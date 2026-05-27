// micro-PoC：探 libcuobjclient 的直通 token API 在我们关心的两个维度的行为。
//
// Q5: cuMemObjGetRDMAToken 在同一 cuObj client 上多线程并发调用是否安全？
//     如果安全 → 阶段 1 主路径数据面无锁；如果不安全 → 加一段短 mutex。
//
// Q6: 不调 cuMemObjPutRDMAToken 直接 cudaFree / 进程退出会怎样？
//     是 nvidia-fs 死锁警告，还是平静退出？决定错误路径 token 兜底策略。
//
// 用法：
//   ./us3_turbo_access_gds_token_probe --endpoint=ip:port [--threads=N] [--iters=M]
// 输出：每个线程的成功/失败计数 + 总耗时；Q6 阶段故意泄漏一个 token 再退出。

#include <cuda_runtime.h>
#include <cufile.h>
#include <cuobjclient.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "client/src/transports/gds/cuobj_library.h"
#include "us3_turbo_access/client/client.h"  // 仅为 Initialize gateway connection

namespace {

bool ParseULL(std::string_view s, std::uint64_t& out) {
  try { std::size_t pos = 0; auto v = std::stoull(std::string(s), &pos, 0);
        if (pos != s.size()) return false; out = v; return true; }
  catch (...) { return false; }
}

// 不会被 cuObjPut/cuObjGet 触发（我们不调它们），仅为构造 cuObjClient 满足
// CUObjOps_t 的非空字段要求。
ssize_t StubGet(const void*, char*, size_t, loff_t, const cufileRDMAInfo_t*) {
  return -1;
}
ssize_t StubPut(const void*, const char*, size_t, loff_t, const cufileRDMAInfo_t*) {
  return -1;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo_access::client;

  std::string endpoint;
  std::uint64_t threads = 4;
  std::uint64_t iters   = 10000;
  std::size_t   buf_sz  = 4ULL * 1024 * 1024;

  for (int i = 1; i < argc; ++i) {
    std::string_view s = argv[i];
    auto eat = [&](std::string_view k) -> std::string_view {
      if (s.size() <= k.size() || s.substr(0, k.size()) != k) return {};
      return s.substr(k.size());
    };
    if (auto v = eat("--endpoint=");!v.empty()) { endpoint = v; continue; }
    std::uint64_t n = 0;
    if (auto v = eat("--threads=");!v.empty() && ParseULL(v, n)) { threads = n; continue; }
    if (auto v = eat("--iters=");  !v.empty() && ParseULL(v, n)) { iters   = n; continue; }
    std::cerr << "unknown arg: " << s << std::endl; return 1;
  }
  if (endpoint.empty()) { std::cerr << "--endpoint required" << std::endl; return 1; }

  // 1. dlopen 出 cuObj API 表
  auto lib = CuObjLibrary::Get();
  if (!lib.success()) {
    std::cerr << "CuObjLibrary::Get failed: " << lib.error().message << std::endl;
    return 1;
  }
  const auto& api = lib.value()->api();

  // 2. 构造一个 cuObjClient 实例（直接走 SDK constructor，不依赖我们的封装）
  CUObjOps_t ops{};
  ops.get = &StubGet;
  ops.put = &StubPut;
  alignas(alignof(cuObjClient)) std::byte storage[std::max(sizeof(cuObjClient), std::size_t{4096})];
  api.constructor(&storage, ops, CUOBJ_PROTO_RDMA_DC_V1);
  if (!api.is_connected(&storage)) {
    std::cerr << "cuObjClient not connected to RDMA service" << std::endl;
    return 1;
  }

  // 3. 一份 device buffer + register
  void* dev = nullptr;
  if (cudaMalloc(&dev, buf_sz) != cudaSuccess) {
    std::cerr << "cudaMalloc failed" << std::endl; return 1;
  }
  if (api.get_descriptor(&storage, dev, buf_sz) != CU_OBJ_SUCCESS) {
    std::cerr << "cuMemObjGetDescriptor failed" << std::endl; return 1;
  }
  std::cerr << "[setup] client connected, buffer registered ("
            << buf_sz << " B)" << std::endl;

  // 4. Q5：N 线程并发 GetRDMAToken/PutRDMAToken，不同 offset 避免 trivial dedup
  std::cerr << "[Q5] launching " << threads << " threads × " << iters
            << " iterations of GetRDMAToken/PutRDMAToken on shared client..."
            << std::endl;

  std::vector<std::uint64_t> ok(threads, 0);
  std::vector<std::uint64_t> err(threads, 0);
  std::atomic<bool> first_err_logged{false};

  const std::size_t step = std::max<std::size_t>(4096, buf_sz / 16);
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> ths;
  for (std::uint64_t t = 0; t < threads; ++t) {
    ths.emplace_back([&, t]() {
      for (std::uint64_t i = 0; i < iters; ++i) {
        const std::size_t off = (i * step) % buf_sz;
        const std::size_t sz  = std::min<std::size_t>(step, buf_sz - off);
        char* tok = nullptr;
        auto rc = api.get_rdma_token(&storage, dev, sz, off, CUOBJ_PUT, &tok);
        if (rc != CU_OBJ_SUCCESS || tok == nullptr) {
          ++err[t];
          if (!first_err_logged.exchange(true)) {
            std::cerr << "[Q5] first failure t=" << t << " i=" << i
                      << " rc=" << rc << " tok=" << static_cast<void*>(tok)
                      << std::endl;
          }
          continue;
        }
        ++ok[t];
        api.put_rdma_token(&storage, tok);
      }
    });
  }
  for (auto& th : ths) th.join();
  auto t1 = std::chrono::steady_clock::now();
  const double wall_s = std::chrono::duration<double>(t1 - t0).count();

  std::uint64_t total_ok = 0, total_err = 0;
  for (std::uint64_t t = 0; t < threads; ++t) {
    std::cerr << "  t=" << t << " ok=" << ok[t] << " err=" << err[t] << std::endl;
    total_ok += ok[t]; total_err += err[t];
  }
  std::cerr << "[Q5] total ok=" << total_ok << " err=" << total_err
            << " wall=" << wall_s << " s, rate="
            << (total_ok / wall_s) << " tokens/s" << std::endl;
  if (total_err > 0) {
    std::cerr << "[Q5] VERDICT: NOT thread-safe (need mutex)" << std::endl;
  } else {
    std::cerr << "[Q5] VERDICT: thread-safe (no mutex needed)" << std::endl;
  }

  // 5. Q6：故意泄漏一个 token，直接退出（不 put_rdma_token，不 put_descriptor，
  //    甚至不 cudaFree）。观察 nvidia-fs 是否报错 / 死锁。
  char* leaked = nullptr;
  auto rc = api.get_rdma_token(&storage, dev, buf_sz, 0, CUOBJ_PUT, &leaked);
  std::cerr << "[Q6] intentionally leaking 1 token (rc=" << rc
            << " tok=" << static_cast<void*>(leaked)
            << "). Exiting without PutRDMAToken / PutDescriptor / cudaFree."
            << std::endl;
  std::cerr << "[Q6] If you see nvidia-fs deadlock warning after this line, "
               "the test failed Q6." << std::endl;

  // 故意 leak. 不调 api.put_rdma_token / put_descriptor / destructor / cudaFree.
  return 0;
}
