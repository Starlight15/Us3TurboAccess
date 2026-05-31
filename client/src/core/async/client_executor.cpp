#include "client/src/core/async/client_executor.h"

#include <thread>

namespace us3_turbo_access::client {

// worker_threads 参数保留为兼容旧 API；实际并发由 brpc 全局 bthread pool 决定。
ClientExecutor::ClientExecutor(std::size_t /*worker_threads*/) {}

ClientExecutor::~ClientExecutor() {
  // 兜底：调用方应该已经显式 Shutdown(grace) 过；若没有则给一个合理 grace。
  // grace=5s：足够正常网络请求收尾，比之前 30s 强退低很多
  // （30s 是问题：用户重启就要等 30s）。
  // 仍 inflight 时会累加 unclean_shutdowns_ 计数，下次诊断时可以查。
  (void)Shutdown(std::chrono::seconds(5));
}

bool ClientExecutor::Shutdown(std::chrono::milliseconds grace) {
  // Submit 后立刻 set stop_=true 阻止新提交。
  stop_.store(true, std::memory_order_release);

  // 等所有 inflight bthread 跑完；grace 内排空就 return true。
  using clk = std::chrono::steady_clock;
  const auto deadline = clk::now() + grace;
  while (inflight_.load(std::memory_order_acquire) > 0 &&
         clk::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const auto remaining = inflight_.load(std::memory_order_acquire);
  if (remaining > 0) {
    // grace 超时但还有 inflight：累计计数，不再等。
    // 这种情况下 bthread 可能在本对象销毁后访问 inflight_/stop_ → UB。
    unclean_shutdowns_.fetch_add(1, std::memory_order_acq_rel);
    return false;
  }
  return true;
}

void* ClientExecutor::TaskEntry(void* arg) {
  auto* boxed = static_cast<std::function<void()>*>(arg);
  (*boxed)();
  delete boxed;
  return nullptr;
}

}  // namespace us3_turbo_access::client
