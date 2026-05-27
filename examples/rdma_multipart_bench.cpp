// Native RDMA multipart 性能 bench：与 http_multipart_bench 同接口。
//
// 每 part 一份独立 PinnedBuffer（避免 N 个 worker 同时 reg 同一段 MR 的并发
// 风险；RDMA 路径每次 PUT 现场 reg 一次本地 MR）。所有 part 共享一段 host
// 内 payload 模板（reduce 内存），但 PinnedBuffer 容器各持。

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
#include "us3_turbo_access/client/pinned_buffer.h"

namespace {
using namespace us3_turbo_access::client;

struct Args {
  std::string endpoint;
  std::size_t threads      = 0;
  std::size_t count        = 4;
  std::size_t object_size  = 32ULL * 1024 * 1024;
  std::size_t part_size    = 8ULL  * 1024 * 1024;   // >= server min_part_size 5 MiB
  std::size_t warmup       = 1;
  std::string bucket       = "us3-bench";
  std::string key_prefix   = "bench/rdma-mp/";
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

void FillRandom(std::byte* p, std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) { const auto v = rng(); std::memcpy(p + i, &v, 8); }
  if (i < n) { const auto v = rng(); std::memcpy(p + i, &v, n - i); }
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!ParseArgs(argc, argv, a)) return 1;

  ClientOptions options;
  options.endpoint  = a.endpoint;
  options.client_id = "us3-rdma-multipart-bench";
  options.data_path = DataPath::kNativeRdma;
  options.async_worker_threads = a.threads;
  Client client(std::move(options));
  if (auto init = client.Initialize(); !init.success()) {
    std::cerr << "Initialize failed: " << init.error().message << std::endl;
    return 1;
  }

  // 为每个 part 单独分配 PinnedBuffer（RDMA 通路每次 reg 本地 MR）。
  const std::size_t num_parts = (a.object_size + a.part_size - 1) / a.part_size;
  std::vector<PinnedBuffer> part_buffers;
  std::vector<std::size_t>  part_sizes(num_parts);
  part_buffers.reserve(num_parts);
  for (std::size_t i = 0; i < num_parts; ++i) {
    const std::size_t off = i * a.part_size;
    const std::size_t sz  = std::min(a.part_size, a.object_size - off);
    part_sizes[i] = sz;
    auto pb = PinnedBuffer::Allocate(sz);
    if (!pb.success()) {
      std::cerr << "PinnedBuffer alloc part[" << i << "] failed: "
                << pb.error().message << std::endl;
      return 1;
    }
    FillRandom(static_cast<std::byte*>(pb.value().data()), sz, a.seed + i + 1);
    part_buffers.push_back(std::move(pb.value()));
  }

  const std::size_t concurrency = a.threads > 0 ? a.threads : 4;

  auto UploadOne = [&](const std::string& key) -> bool {
    auto start = client.StartUpload(ObjectId{a.bucket, key}, a.object_size);
    if (!start.success()) {
      std::cerr << "StartUpload failed: " << start.error().message << std::endl;
      return false;
    }
    auto& upload = start.value();
    std::vector<MultipartUpload::PartSpec> specs;
    specs.reserve(num_parts);
    for (std::size_t i = 0; i < num_parts; ++i) {
      specs.push_back(MultipartUpload::PartSpec{
          .part_number   = static_cast<std::uint32_t>(i + 1),
          .object_offset = i * a.part_size,
          .buffer = ConstBufferView{.data = part_buffers[i].data(),
                                    .size = part_sizes[i],
                                    .type = BufferType::kHostPinned},
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
  };

  for (std::size_t i = 0; i < a.warmup; ++i) {
    if (!UploadOne(a.key_prefix + "warmup-" + std::to_string(i))) return 1;
  }

  bench::Runner runner({
      .path_label  = "rdma",
      .mode_label  = "multipart",
      .threads     = a.threads,
      .count       = a.count,
      .object_size = a.object_size,
      .warmup      = a.warmup,
  });

  std::size_t failed = 0;
  runner.BeginMeasured();
  for (std::size_t i = 0; i < a.count; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = UploadOne(a.key_prefix + "iter-" + std::to_string(i));
    const auto t1 = std::chrono::steady_clock::now();
    if (!ok) { ++failed; continue; }
    runner.RecordLatency(t0, t1);
  }
  runner.End(failed);

  runner.PrintHuman(std::cerr);
  runner.PrintJson(std::cout);
  return failed == 0 ? 0 : 1;
}
