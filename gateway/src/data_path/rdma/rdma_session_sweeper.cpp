#include "data_path/rdma/rdma_session_sweeper.h"

#include <utility>
#include <vector>

#include "data_path/rdma/rdma_session_registry.h"

namespace us3_turbo_access::gateway::data_path::rdma {

RdmaSessionSweeper::RdmaSessionSweeper(RdmaSessionRegistry& registry,
                                         std::chrono::seconds sweep_interval,
                                         EraseFn erase_fn,
                                         std::shared_ptr<spdlog::logger> logger)
    : registry_(registry),
      sweep_interval_(sweep_interval),
      erase_fn_(std::move(erase_fn)),
      logger_(std::move(logger)) {}

RdmaSessionSweeper::~RdmaSessionSweeper() { Stop(); }

void RdmaSessionSweeper::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
    return;
  }
  thread_ = std::thread(&RdmaSessionSweeper::Run, this);
}

void RdmaSessionSweeper::Stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false,
                                          std::memory_order_acq_rel)) {
    return;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

// 周期性轮询 expired session，调注入的 erase_fn 逐条释放。
// 用 cv::wait_for 让 Stop 能立即唤醒退出，不必等到下一轮 interval。
void RdmaSessionSweeper::Run() {
  while (running_.load(std::memory_order_acquire)) {
    {
      std::unique_lock<std::mutex> lk(mu_);
      // 例外类别 1（docs/code-review-process.md §4.3）：cv_.wait_for 强制要求
      // 一个 nullary callable 返回 bool，标准库没法接 mem-fn ptr。
      cv_.wait_for(lk, sweep_interval_, [this] {
        return !running_.load(std::memory_order_acquire);
      });
      if (!running_.load(std::memory_order_acquire)) break;
    }
    const auto now = std::chrono::steady_clock::now();
    auto expired = registry_.CollectExpired(now);
    if (expired.empty()) continue;
    if (logger_ != nullptr) {
      logger_->info("rdma: sweeper reaping {} expired session(s)",
                    expired.size());
    }
    for (const auto& sid : expired) {
      if (erase_fn_) erase_fn_(sid);
    }
  }
}

}  // namespace us3_turbo_access::gateway::data_path::rdma
