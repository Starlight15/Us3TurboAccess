#include "runtime/io_worker_pool.h"

#include <utility>

namespace us3_turbo_access::gateway::runtime {

IoWorkerPool::IoWorkerPool(std::size_t worker_count) {
  if (worker_count == 0) {
    worker_count = 1;
  }
  running_.store(true, std::memory_order_release);
  workers_.reserve(worker_count);
  for (std::size_t i = 0; i < worker_count; ++i) {
    workers_.emplace_back(&IoWorkerPool::WorkerLoop, this);
  }
}

IoWorkerPool::~IoWorkerPool() { Stop(); }

void IoWorkerPool::Submit(std::function<void()> task) {
  if (!running_.load(std::memory_order_acquire)) {
    // 兜底：pool 已停，inline 执行，避免 RPC 永远挂着。
    task();
    return;
  }
  bool was_empty;
  {
    std::scoped_lock lock(mu_);
    was_empty = queue_.empty();
    queue_.push_back(std::move(task));
  }
  // 队列从空变非空时唤醒所有 worker，否则只唤醒一个
  if (was_empty) {
    cv_.notify_all();
  } else {
    cv_.notify_one();
  }
}

void IoWorkerPool::Stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) {
    return;
  }
  cv_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}

// 取任务并执行；Stop 后排空队列再退出，保证 in-flight 任务能 fire done closure。
void IoWorkerPool::WorkerLoop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock lock(mu_);
      while (running_.load(std::memory_order_acquire) && queue_.empty()) {
        cv_.wait(lock);
      }
      if (!queue_.empty()) {
        task = std::move(queue_.front());
        queue_.pop_front();
      } else {
        // 已停且队列空，退出。
        return;
      }
    }
    // 锁外执行任务，多 worker 真正并发。
    task();
  }
}

}  // namespace us3_turbo_access::gateway::runtime
