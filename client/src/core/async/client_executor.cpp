#include "client/src/core/async/client_executor.h"

namespace us3_turbo_access::client {

ClientExecutor::ClientExecutor(std::size_t worker_threads) {
  if (worker_threads == 0) worker_threads = 1;
  workers_.reserve(worker_threads);
  for (std::size_t i = 0; i < worker_threads; ++i) {
    workers_.emplace_back(&ClientExecutor::WorkerLoop, this);
  }
}

ClientExecutor::~ClientExecutor() {
  {
    std::scoped_lock lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
}

// drain：stop 后仍跑完已入队任务，避免 future 永远悬挂。
void ClientExecutor::WorkerLoop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lk(mu_);
      // 例外类别 1（docs/code-review-process.md §4.3）：cv_.wait 强制要求
      // 一个 nullary callable 返回 bool，标准库没法接 mem-fn ptr。
      cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
      if (tasks_.empty()) return;  // stop_ 且队列已空
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    task();
  }
}

}  // namespace us3_turbo_access::client
