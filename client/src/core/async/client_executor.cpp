#include "client/src/core/async/client_executor.h"

#include <chrono>
#include <thread>

namespace us3_turbo_access::client {

// worker_threads 参数保留为兼容旧 API；实际并发由 brpc 全局 bthread pool 决定。
ClientExecutor::ClientExecutor(std::size_t /*worker_threads*/) {}

ClientExecutor::~ClientExecutor() {
  stop_.store(true, std::memory_order_release);
  // 等所有 inflight bthread 跑完；正常情况下析构前调用方应该都 future.get()
  // 过了，这里只是兜底防止 bthread 在 ClientExecutor 销毁后访问 inflight_。
  // 短超时轮询：单笔任务一般 < 几百 ms。
  using clk = std::chrono::steady_clock;
  const auto deadline = clk::now() + std::chrono::seconds(30);
  while (inflight_.load(std::memory_order_acquire) > 0 &&
         clk::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void* ClientExecutor::TaskEntry(void* arg) {
  auto* boxed = static_cast<std::function<void()>*>(arg);
  (*boxed)();
  delete boxed;
  return nullptr;
}

}  // namespace us3_turbo_access::client
