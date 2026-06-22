// Native RDMA 整对象 PUT 性能 bench：与 http_put_bench 同接口、同 JSON。
//
// UCX TAG 路径不需要 pinned buffer（无 ibv_reg_mr），用普通对齐内存。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "examples/common/bench_runner.h"
#include "us3_turbo_access/client/client.h"
#include <cstdlib>
#include <sys/mman.h>

namespace {
using namespace us3_turbo_access::client;

struct Args {
  std::string endpoint;
  std::size_t threads      = 0;
  std::size_t count        = 64;
  std::size_t object_size  = 4ULL * 1024 * 1024;
  std::size_t warmup       = 4;
  std::size_t key_modulo   = 0;
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
    if (auto v = eat("--key-modulo=");!v.empty() && ParseULL(v, n))  { a.key_modulo = n; continue; }
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
  options.data_flow = DataFlow::CPUDirect;
  options.async_worker_threads = a.threads;
  options.rdma.send_crc32c     = false;  // 专注 throughput
  Client client(std::move(options));
  if (auto init = client.Initialize(); !init.success()) {
    std::cerr << "Initialize failed: " << init.error().message << std::endl;
    return 1;
  }

  // UCX TAG 路径使用普通对齐内存（无需 cudaHostAlloc）
  void* raw_buf = std::aligned_alloc(4096, a.object_size);
  if (!raw_buf) {
    std::cerr << "aligned_alloc failed" << std::endl;
    return 1;
  }
  ::mlock(raw_buf, a.object_size);  // best-effort pin
  FillRandom(static_cast<std::byte*>(raw_buf), a.object_size, a.seed);
  ConstBufferView buf_view{.data = raw_buf,
                           .size = a.object_size,
                           .type = BufferType::kHostRegular};

  // key 模数：0=每次唯一 key；>0=idx % N 限制后端容量。
  const std::size_t key_mod = a.key_modulo > 0 ? a.key_modulo : a.count;
  auto submit = [&](std::size_t idx) -> std::future<Result<TransferOutcome>> {
    PutObjectRequest req;
    req.bucket = a.bucket;
    req.key = a.key_prefix + std::to_string(idx % key_mod);
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

  /* Bounded in-flight 模型：N 个 worker 同步循环
   *   t0 = now(); PutObject(...).get(); t1 = now(); RecordLatency(t0,t1)
   * 单笔 latency = 真实端到端 RPC 时延（不被 submit-all 队列尾巴污染）。
   * N = a.threads（如果为 0 则默认 4）；总吞吐 = sum(N 路并发)。
   */
  const std::size_t workers = a.threads > 0 ? a.threads : 4;
  std::vector<std::chrono::steady_clock::time_point> starts(a.count);
  std::vector<std::chrono::steady_clock::time_point> ends(a.count);
  std::vector<bool> ok(a.count, false);
  std::atomic<std::size_t> next_idx{0};
  std::atomic<std::size_t> fail_count{0};
  std::mutex err_mu;
  std::string first_err;

  auto worker = [&]() {
    while (true) {
      const auto i = next_idx.fetch_add(1, std::memory_order_acq_rel);
      if (i >= a.count) return;
      starts[i] = std::chrono::steady_clock::now();
      // 同步等待，避免 submit-all 队列污染单笔时延
      auto fut = submit(i);
      auto r   = fut.get();
      ends[i]  = std::chrono::steady_clock::now();
      if (r.success()) {
        ok[i] = true;
      } else {
        fail_count.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lk(err_mu);
        if (first_err.empty()) first_err = r.error().message;
      }
    }
  };

  runner.BeginMeasured();
  std::vector<std::thread> ths;
  ths.reserve(workers);
  for (std::size_t i = 0; i < workers; ++i) ths.emplace_back(worker);
  for (auto& t : ths) t.join();
  for (std::size_t i = 0; i < a.count; ++i) {
    if (ok[i]) runner.RecordLatency(starts[i], ends[i]);
  }
  runner.End(fail_count.load());
  if (fail_count.load() > 0) {
    std::cerr << "PUT failures=" << fail_count.load()
              << " first_err=" << first_err << std::endl;
  }

  runner.PrintHuman(std::cerr);
  runner.PrintJson(std::cout);
  return fail_count.load() == 0 ? 0 : 1;
}
