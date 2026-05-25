#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace us3_turbo_access::gateway::runtime {

/**
 * @brief 固定大小的后台 worker 线程池，用来跑长阻塞 IO（cuObjServer RDMA、
 *        future RDMA、backend 落盘等），把 brpc worker 释放给"接 RPC + 解包"。
 *
 * 启动后 N 个线程从一个 FIFO 队列里取任务。任务用 std::function<void()> 包装，
 * 调用方负责把 brpc::ClosureGuard / response pointer 等通过 lambda 捕获带进去。
 *
 * 注意：任务函数自己负责终结所属的 brpc RPC（fire done_guard），pool 不参与。
 */
class IoWorkerPool {
 public:
  explicit IoWorkerPool(std::size_t worker_count);
  ~IoWorkerPool();

  IoWorkerPool(const IoWorkerPool&) = delete;
  IoWorkerPool& operator=(const IoWorkerPool&) = delete;

  /**
   * @brief 异步提交任务。stop 之后调用直接 inline 执行（兜底）。
   */
  void Submit(std::function<void()> task);

  /**
   * @brief 停止 pool；唤醒所有线程并 join。剩余未取出任务会被丢弃。
   */
  void Stop();

  [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }

 private:
  void WorkerLoop();

  std::vector<std::thread>              workers_;
  std::mutex                            mu_;
  std::condition_variable               cv_;
  std::deque<std::function<void()>>     queue_;
  std::atomic<bool>                     running_{false};
};

}  // namespace us3_turbo_access::gateway::runtime
