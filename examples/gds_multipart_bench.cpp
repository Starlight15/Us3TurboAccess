// GDS (cuObject) multipart 性能 bench：与 http/rdma multipart bench 同接口。
//
// 每 part 一份 cudaMalloc（与 RDMA bench 对称）；cuObj 内部串行化 PUT，
// 多 part 并发提交主要受限于该串行点 + control plane 排队。

#include <cuda_runtime.h>

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
  std::size_t count        = 4;
  std::size_t object_size  = 32ULL * 1024 * 1024;
  std::size_t part_size    = 8ULL  * 1024 * 1024;
  std::size_t warmup       = 1;
  std::string bucket       = "us3-bench";
  std::string key_prefix   = "bench/gds-mp/";
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

bool CheckCuda(cudaError_t s, const char* tag) {
  if (s == cudaSuccess) return true;
  std::cerr << tag << " failed: " << cudaGetErrorString(s) << std::endl;
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!ParseArgs(argc, argv, a)) return 1;

  // 每个 part 一份 device buffer
  const std::size_t num_parts = (a.object_size + a.part_size - 1) / a.part_size;
  std::vector<void*>       dev_parts(num_parts, nullptr);
  std::vector<std::size_t> part_sizes(num_parts);
  std::vector<std::byte>   host_seed(a.part_size);
  std::mt19937_64 rng(a.seed);
  for (std::size_t i = 0; i < num_parts; ++i) {
    const std::size_t off = i * a.part_size;
    const std::size_t sz  = std::min(a.part_size, a.object_size - off);
    part_sizes[i] = sz;
    if (!CheckCuda(cudaMalloc(&dev_parts[i], sz), "cudaMalloc part")) {
      for (auto* p : dev_parts) if (p) cudaFree(p);
      return 1;
    }
    // 填 host_seed 前 sz 字节
    auto* p = host_seed.data();
    std::size_t k = 0;
    rng.seed(a.seed + i + 1);
    for (; k + 8 <= sz; k += 8) { const auto v = rng(); std::memcpy(p + k, &v, 8); }
    if (k < sz) { const auto v = rng(); std::memcpy(p + k, &v, sz - k); }
    if (!CheckCuda(cudaMemcpy(dev_parts[i], host_seed.data(), sz,
                                cudaMemcpyHostToDevice), "cudaMemcpy")) {
      for (auto* q : dev_parts) if (q) cudaFree(q);
      return 1;
    }
  }
  cudaDeviceSynchronize();

  ClientOptions options;
  options.endpoint  = a.endpoint;
  options.client_id = "us3-gds-multipart-bench";
  options.data_path = DataPath::kGdsCuObject;
  options.async_worker_threads = a.threads;
  Client client(std::move(options));
  if (auto init = client.Initialize(); !init.success()) {
    std::cerr << "Initialize failed: " << init.error().message << std::endl;
    for (auto* q : dev_parts) if (q) cudaFree(q);
    return 1;
  }

  // 预注册每个 part 的 device buffer 到 cuObj descriptor 表，避免热路径
  // 上 lazy register 的毫秒级 nvidia_p2p_get_pages 开销。
  for (std::size_t i = 0; i < num_parts; ++i) {
    auto r = client.RegisterDeviceBuffer(dev_parts[i], part_sizes[i]);
    if (!r.success()) {
      std::cerr << "RegisterDeviceBuffer failed: " << r.error().message
                << std::endl;
      for (auto* q : dev_parts) if (q) cudaFree(q);
      return 1;
    }
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
          .buffer = ConstBufferView{.data = dev_parts[i],
                                    .size = part_sizes[i],
                                    .type = BufferType::kCudaDevice},
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
    if (!UploadOne(a.key_prefix + "warmup-" + std::to_string(i))) {
      for (auto* q : dev_parts) if (q) cudaFree(q);
      return 1;
    }
  }

  bench::Runner runner({
      .path_label  = "gds",
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

  for (std::size_t i = 0; i < num_parts; ++i) {
    if (dev_parts[i]) (void)client.UnregisterDeviceBuffer(dev_parts[i]);
  }
  for (auto* q : dev_parts) if (q) cudaFree(q);
  return failed == 0 ? 0 : 1;
}
