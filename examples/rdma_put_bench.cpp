// Native RDMA 整对象 PUT 性能 bench：与 http_put_bench 同接口、同 JSON。
//
// RDMA 要求 kHostPinned buffer；这里 cudaHostAlloc 出一个共享 buffer，
// N 个 worker（ClientExecutor）并发 PutObjectAsync。RDMA QP / completion
// driver / connection pool 在 V2 已落地，本 bench 不去 tweak 它们。

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
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
  std::size_t count        = 64;
  std::size_t object_size  = 4ULL * 1024 * 1024;
  std::size_t warmup       = 4;
  std::string bucket       = "us3-bench";
  std::string key_prefix   = "bench/rdma-put/";
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
    if (auto v = eat("--warmup=");     !v.empty() && ParseULL(v, n)) { a.warmup = n; continue; }
    if (auto v = eat("--seed=");       !v.empty() && ParseULL(v, n)) { a.seed = n; continue; }
    std::cerr << "unknown arg: " << s << std::endl;
    return false;
  }
  if (a.endpoint.empty()) { std::cerr << "--endpoint required" << std::endl; return false; }
  if (a.count == 0 || a.object_size == 0) { std::cerr << "--count / --object-size > 0" << std::endl; return false; }
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
  options.client_id = "us3-rdma-put-bench";
  options.data_path = DataPath::kNativeRdma;
  options.async_worker_threads = a.threads;
  Client client(std::move(options));
  if (auto init = client.Initialize(); !init.success()) {
    std::cerr << "Initialize failed: " << init.error().message << std::endl;
    return 1;
  }

  // pinned buffer 共享给 N 个并发 PUT；buffer 在整段 bench 生命周期内有效，
  // RDMA worker 同步 PUT 一直等 WRITE 完成回 outcome 才返回。
  auto pin = PinnedBuffer::Allocate(a.object_size);
  if (!pin.success()) {
    std::cerr << "PinnedBuffer::Allocate failed: " << pin.error().message << std::endl;
    return 1;
  }
  FillRandom(static_cast<std::byte*>(pin.value().data()), a.object_size, a.seed);
  ConstBufferView buf_view{.data = pin.value().data(),
                           .size = a.object_size,
                           .type = BufferType::kHostPinned};

  auto submit = [&](std::size_t idx) -> std::future<Result<TransferOutcome>> {
    RequestOptions req;
    req.object = ObjectId{.bucket = a.bucket,
                          .key = a.key_prefix + std::to_string(idx)};
    req.length = a.object_size;
    return client.PutObjectAsync(req, buf_view);
  };

  // warm-up
  if (a.warmup > 0) {
    std::vector<std::future<Result<TransferOutcome>>> wfs;
    wfs.reserve(a.warmup);
    for (std::size_t i = 0; i < a.warmup; ++i) {
      wfs.push_back(submit(0xFFFF0000ULL + i));
    }
    for (auto& f : wfs) {
      auto r = f.get();
      if (!r.success()) {
        std::cerr << "warmup PUT failed: " << r.error().message << std::endl;
        return 1;
      }
    }
  }

  bench::Runner runner({
      .path_label  = "rdma",
      .mode_label  = "put",
      .threads     = a.threads,
      .count       = a.count,
      .object_size = a.object_size,
      .warmup      = a.warmup,
  });
  std::vector<std::future<Result<TransferOutcome>>> futures;
  std::vector<std::chrono::steady_clock::time_point> starts(a.count);
  futures.reserve(a.count);

  runner.BeginMeasured();
  for (std::size_t i = 0; i < a.count; ++i) {
    starts[i] = std::chrono::steady_clock::now();
    futures.push_back(submit(i));
  }
  std::size_t failed = 0;
  for (std::size_t i = 0; i < a.count; ++i) {
    auto r = futures[i].get();
    const auto t1 = std::chrono::steady_clock::now();
    if (!r.success()) {
      ++failed;
      std::cerr << "PUT " << i << " failed: " << r.error().message << std::endl;
      continue;
    }
    runner.RecordLatency(starts[i], t1);
  }
  runner.End(failed);

  runner.PrintHuman(std::cerr);
  runner.PrintJson(std::cout);
  return failed == 0 ? 0 : 1;
}
