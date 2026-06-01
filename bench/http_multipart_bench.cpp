// HTTP multipart 上传性能 bench：每轮上传 1 个对象（StartUpload → N 并发
// UploadPart → Complete），统计端到端 latency 与吞吐。
//
// 设计：
//   --threads N        客户端线程池（ClientExecutor），同时 UploadParts 的内部
//                      并发上限。一个对象内 part 并发 = N。
//   --object-size B    单对象总字节数。
//   --part-size B      单 part 字节数（>= server min_part_size 5 MiB）。
//   --count N          测量轮数（每轮 1 个对象）。
//   --warmup N         warmup 轮数。
//   CPU 限制走外部 taskset。
//
// 输出：stderr 人读，stdout 一行 JSON（见 bench_runner.h）。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "examples/common/bench_runner.h"
#include "us3_turbo_access/client/client.h"

namespace {
using namespace us3_turbo_access::client;

struct Args {
  std::string endpoint;
  std::size_t threads      = 0;
  std::size_t count        = 8;
  std::size_t object_size  = 32ULL * 1024 * 1024;
  std::size_t part_size    = 8ULL  * 1024 * 1024;   // ≥ server min_part_size 5 MiB
  std::size_t warmup       = 2;
  std::size_t key_modulo   = 0;
  std::string bucket       = "us3-bench";
  std::string key_prefix   = "bench/http-mp/";
  std::uint64_t seed       = 0xC0FFEEULL;
};

bool ParseULL(std::string_view s, std::uint64_t& out) {
  try { std::size_t pos = 0; auto v = std::stoull(std::string(s), &pos, 0);
        if (pos != s.size()) return false; out = v; return true; }
  catch (...) { return false; }
}

bool ParseArgs(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    std::string_view s = argv[i];
    auto eat = [&](std::string_view k) -> std::string_view {
      if (s.size() <= k.size() || s.substr(0, k.size()) != k) return {};
      return s.substr(k.size());
    };
    if (auto v = eat("--endpoint=");   !v.empty()) { a.endpoint = v; continue; }
    if (auto v = eat("--bucket=");     !v.empty()) { a.bucket   = v; continue; }
    if (auto v = eat("--key-prefix=");!v.empty())  { a.key_prefix = v; continue; }
    std::uint64_t n = 0;
    if (auto v = eat("--threads=");    !v.empty() && ParseULL(v, n)) { a.threads = n; continue; }
    if (auto v = eat("--count=");      !v.empty() && ParseULL(v, n)) { a.count = n; continue; }
    if (auto v = eat("--object-size=");!v.empty() && ParseULL(v, n)) { a.object_size = n; continue; }
    if (auto v = eat("--part-size=");  !v.empty() && ParseULL(v, n)) { a.part_size = n; continue; }
    if (auto v = eat("--warmup=");     !v.empty() && ParseULL(v, n)) { a.warmup = n; continue; }
    if (auto v = eat("--key-modulo=");!v.empty() && ParseULL(v, n))  { a.key_modulo = n; continue; }
    if (auto v = eat("--seed=");       !v.empty() && ParseULL(v, n)) { a.seed = n; continue; }
    std::cerr << "unknown arg: " << s << std::endl;
    return false;
  }
  if (a.endpoint.empty()) { std::cerr << "--endpoint required" << std::endl; return false; }
  if (a.count == 0 || a.object_size == 0 || a.part_size == 0) {
    std::cerr << "--count / --object-size / --part-size > 0" << std::endl; return false;
  }
  return true;
}

std::vector<std::byte> MakePayload(std::size_t n, std::uint64_t seed) {
  std::vector<std::byte> buf(n);
  std::mt19937_64 rng(seed);
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) { const auto v = rng(); std::memcpy(buf.data() + i, &v, 8); }
  if (i < n) { const auto v = rng(); std::memcpy(buf.data() + i, &v, n - i); }
  return buf;
}

// 跑一个对象的 multipart：返回是否成功。失败信息打到 stderr。
bool UploadOneObject(Client& client, const std::string& bucket,
                       const std::string& key, std::size_t object_size,
                       std::size_t part_size, std::size_t concurrency,
                       const std::vector<std::byte>& payload) {
  auto start = client.StartUpload(ObjectId{bucket, key}, object_size);
  if (!start.success()) {
    std::cerr << "StartUpload failed: " << start.error().message << std::endl;
    return false;
  }
  auto& upload = start.value();
  const std::size_t num_parts = (object_size + part_size - 1) / part_size;

  std::vector<MultipartUpload::PartSpec> specs;
  specs.reserve(num_parts);
  for (std::size_t i = 0; i < num_parts; ++i) {
    const std::size_t off = i * part_size;
    const std::size_t sz  = std::min(part_size, object_size - off);
    specs.push_back(MultipartUpload::PartSpec{
        .part_number   = static_cast<std::uint32_t>(i + 1),
        .object_offset = off,
        .buffer = ConstBufferView{.data = payload.data() + off,
                                  .size = sz,
                                  .type = BufferType::kHostRegular},
    });
  }
  auto up = upload.UploadParts(specs, std::min(concurrency, num_parts));
  if (!up.success()) {
    std::cerr << "UploadParts failed: " << up.error().message << std::endl;
    return false;
  }
  auto cmp = upload.Complete();
  if (!cmp.success()) {
    std::cerr << "Complete failed: " << cmp.error().message << std::endl;
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!ParseArgs(argc, argv, a)) return 1;

  ClientOptions options;
  options.endpoint  = a.endpoint;
  options.client_id = "us3-http-multipart-bench";
  options.data_path = DataPath::kHttpTcp;
  options.async_worker_threads = a.threads;
  options.http.send_crc32c            = false;
  options.http.verify_response_crc32c = false;
  Client client(std::move(options));
  if (auto init = client.Initialize(); !init.success()) {
    std::cerr << "Initialize failed: " << init.error().message << std::endl;
    return 1;
  }

  // payload 共享：所有 PartSpec 引用 payload 的不同 offset；buffer 生命周期
  // 覆盖整个 multipart 调用栈。
  const auto payload = MakePayload(a.object_size, a.seed);

  // 并发 part 数 = min(threads, num_parts)；这里取 threads（runner 自动 cap）。
  const std::size_t concurrency =
      a.threads > 0 ? a.threads : 4;

  // ---- warm-up ----
  for (std::size_t i = 0; i < a.warmup; ++i) {
    const std::string key = a.key_prefix + "warmup-" + std::to_string(i);
    if (!UploadOneObject(client, a.bucket, key, a.object_size, a.part_size,
                           concurrency, payload)) {
      return 1;
    }
  }

  // ---- 正式测量 ----
  bench::Runner runner({
      .path_label  = "http",
      .mode_label  = "multipart",
      .threads     = a.threads,
      .count       = a.count,
      .object_size = a.object_size,
      .warmup      = a.warmup,
  });

  const std::size_t key_mod = a.key_modulo > 0 ? a.key_modulo : a.count;
  std::size_t failed = 0;
  runner.BeginMeasured();
  for (std::size_t i = 0; i < a.count; ++i) {
    const std::string key = a.key_prefix + "iter-" + std::to_string(i % key_mod);
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = UploadOneObject(client, a.bucket, key, a.object_size,
                                       a.part_size, concurrency, payload);
    const auto t1 = std::chrono::steady_clock::now();
    if (!ok) { ++failed; continue; }
    runner.RecordLatency(t0, t1);
  }
  runner.End(failed);

  runner.PrintHuman(std::cerr);
  runner.PrintJson(std::cout);
  return failed == 0 ? 0 : 1;
}
