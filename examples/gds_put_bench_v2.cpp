// GDS (cuObject) 整对象 PUT 性能 bench。与 http_put_bench / rdma_put_bench
// 同接口、同 JSON。
//
// 关键差异：cuObj/cuFile 在每个工作线程里都要先 cudaSetDevice，否则
// cuMemObjGetDescriptor 会失败；Client::PutObjectAsync 走的 ClientExecutor
// worker 没设置 device，所以 GDS 不走 *Async，本 bench 自己起一个固定 N
// 的 worker 池，每个线程先 cudaSetDevice 再调同步 PutObject。
// 另外，cuObjClient SDK 的 descriptor 表是 process-wide，并发 PUT 复用同
// 一段 device pointer 会注册第二次失败，所以预分配 pool_size 份独立
// cudaMalloc，按 (idx % pool_size) 轮转使用。
//
// 用法与其他 bench 完全一致；CPU 限制走外部 taskset。

#include <cuda_runtime.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "examples/common/bench_runner.h"
#include "us3_turbo_access/client/client.h"

namespace {
using namespace us3_turbo_access::client;

struct Args {
  std::string endpoint;
  std::size_t threads      = 0;
  std::size_t count        = 16;
  std::size_t object_size  = 4ULL * 1024 * 1024;
  std::size_t warmup       = 2;
  std::size_t key_modulo   = 0;  // 0 = unique key per iter; N>0 = idx % N
  std::string bucket       = "us3-bench";
  std::string key_prefix   = "bench/gds-put/";
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

bool CheckCuda(cudaError_t s, const char* tag) {
  if (s == cudaSuccess) return true;
  std::cerr << tag << " failed: " << cudaGetErrorString(s) << std::endl;
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!ParseArgs(argc, argv, a)) return 1;

  // pool_size = 期望并发 worker 数：让每个 worker 都能拿到一份独立 device buffer。
  const std::size_t pool_size = a.threads > 0
      ? a.threads
      : std::max<unsigned>(1U, std::thread::hardware_concurrency() / 2U);

  // 一份 host_seed 灌进所有 device buffer。
  std::vector<std::byte> host_seed(a.object_size);
  {
    std::mt19937_64 rng(a.seed);
    std::size_t i = 0;
    auto* p = host_seed.data();
    for (; i + 8 <= a.object_size; i += 8) { const auto v = rng(); std::memcpy(p + i, &v, 8); }
    if (i < a.object_size) { const auto v = rng(); std::memcpy(p + i, &v, a.object_size - i); }
  }
  std::vector<void*> dev_pool(pool_size, nullptr);
  auto free_pool = [&]() { for (auto* p : dev_pool) if (p) cudaFree(p); };
  for (std::size_t i = 0; i < pool_size; ++i) {
    if (!CheckCuda(cudaMalloc(&dev_pool[i], a.object_size), "cudaMalloc")) {
      free_pool(); return 1;
    }
    if (!CheckCuda(cudaMemcpy(dev_pool[i], host_seed.data(), a.object_size,
                                cudaMemcpyHostToDevice), "cudaMemcpy H2D")) {
      free_pool(); return 1;
    }
  }
  cudaDeviceSynchronize();

  ClientOptions options;
  options.endpoint  = a.endpoint;
  options.client_id = "us3-gds-put-bench";
  options.data_path = DataPath::kGdsCuObject;
  Client client(std::move(options));
  if (auto init = client.Initialize(); !init.success()) {
    std::cerr << "Initialize failed: " << init.error().message << std::endl;
    free_pool();
    return 1;
  }

  // 预先把 pool 内每份 device buffer 注册到 cuObj descriptor 表，避免
  // 热路径上的 lazy register 抖动。
  for (std::size_t i = 0; i < pool_size; ++i) {
    auto r = client.RegisterDeviceBuffer(dev_pool[i], a.object_size);
    if (!r.success()) {
      std::cerr << "RegisterDeviceBuffer failed: " << r.error().message
                << std::endl;
      free_pool();
      return 1;
    }
  }

  // 捕获主线程当前 device；每个 worker 线程入口处 cudaSetDevice。
  int main_device = 0;
  (void)cudaGetDevice(&main_device);

  // key 模数：0 表示每次唯一 key（总存储无界）；>0 表示 idx % N，限制 N
  // 个不同对象循环 overwrite，避免长时间测试把后端容量吃满。
  const std::size_t key_mod = a.key_modulo > 0 ? a.key_modulo : a.count;
  auto do_put = [&](std::size_t idx) -> Result<TransferOutcome> {
    RequestOptions req;
    req.object = ObjectId{.bucket = a.bucket,
                          .key = a.key_prefix + std::to_string(idx % key_mod)};
    req.length = a.object_size;
    void* dev = dev_pool[idx % pool_size];
    ConstBufferView v{.data = dev, .size = a.object_size,
                       .type = BufferType::kCudaDevice};
    return client.PutObject(req, v);
  };

  if (a.warmup > 0) {
    std::atomic<std::size_t> next{0};
    std::mutex mu;
    bool failed_warm = false;
    std::string err_msg;
    auto worker = [&]() {
      if (cudaSetDevice(main_device) != cudaSuccess) {
        std::scoped_lock lk(mu); failed_warm = true;
        err_msg = "cudaSetDevice failed"; return;
      }
      while (true) {
        const auto i = next.fetch_add(1, std::memory_order_acq_rel);
        if (i >= a.warmup) return;
        auto r = do_put(0xFFFF0000ULL + i);
        if (!r.success()) {
          std::scoped_lock lk(mu);
          if (!failed_warm) { failed_warm = true; err_msg = r.error().message; }
          return;
        }
      }
    };
    std::vector<std::thread> ths;
    for (std::size_t i = 0; i < pool_size; ++i) ths.emplace_back(worker);
    for (auto& t : ths) t.join();
    if (failed_warm) {
      std::cerr << "warmup PUT failed: " << err_msg << std::endl;
      free_pool();
      return 1;
    }
  }

  bench::Runner runner({
      .path_label  = "gds",
      .mode_label  = "put",
      .threads     = a.threads,
      .count       = a.count,
      .object_size = a.object_size,
      .warmup      = a.warmup,
  });

  std::vector<std::chrono::steady_clock::time_point> starts(a.count);
  std::vector<std::chrono::steady_clock::time_point> ends(a.count);
  std::vector<bool> ok(a.count, false);
  std::atomic<std::size_t> next_idx{0};
  std::mutex err_mu;
  std::string first_err;

  auto worker = [&]() {
    if (cudaSetDevice(main_device) != cudaSuccess) {
      std::scoped_lock lk(err_mu);
      if (first_err.empty()) first_err = "cudaSetDevice failed";
      return;
    }
    while (true) {
      const auto i = next_idx.fetch_add(1, std::memory_order_acq_rel);
      if (i >= a.count) return;
      starts[i] = std::chrono::steady_clock::now();
      auto r = do_put(i);
      ends[i] = std::chrono::steady_clock::now();
      if (!r.success()) {
        std::scoped_lock lk(err_mu);
        if (first_err.empty()) first_err = r.error().message;
        continue;
      }
      ok[i] = true;
    }
  };

  runner.BeginMeasured();
  std::vector<std::thread> ths;
  ths.reserve(pool_size);
  for (std::size_t i = 0; i < pool_size; ++i) ths.emplace_back(worker);
  for (auto& t : ths) t.join();
  std::size_t failed = 0;
  for (std::size_t i = 0; i < a.count; ++i) {
    if (ok[i]) runner.RecordLatency(starts[i], ends[i]);
    else ++failed;
  }
  runner.End(failed);
  if (failed > 0) {
    std::cerr << "PUT failures=" << failed
              << " first_err=" << first_err << std::endl;
  }

  runner.PrintHuman(std::cerr);
  runner.PrintJson(std::cout);

  // 反注册必须在 cudaFree 之前；free_pool 内部走 cudaFree，所以先 unregister
  for (std::size_t i = 0; i < pool_size; ++i) {
    (void)client.UnregisterDeviceBuffer(dev_pool[i]);
  }
  free_pool();
  return failed == 0 ? 0 : 1;
}
