#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace us3_turbo_access::client {

/**
 * 客户端共享线程池：把同步路径（Put/Get/Head）跑在 worker 上，对外返回
 * std::future，方便上层做 fire-and-forget / 并发等待。
 *
 * 阶段 A 用：等 C5 切到 CQ 完成线程驱动时，本对象会被替换/绕过，但 *Async
 * 对外 API 不变。
 */
class ClientExecutor {
 public:
  explicit ClientExecutor(std::size_t worker_threads);
  ~ClientExecutor();

  ClientExecutor(const ClientExecutor&) = delete;
  ClientExecutor& operator=(const ClientExecutor&) = delete;

  /** Submit 后立即返回 future；任务在某个 worker 上执行。Stop 后再调抛。*/
  template <typename F>
  auto Submit(F&& fn) -> std::future<std::invoke_result_t<F>>;

 private:
  void WorkerLoop();

  std::vector<std::thread>            workers_;
  std::mutex                          mu_;
  std::condition_variable             cv_;
  std::queue<std::function<void()>>   tasks_;
  bool                                stop_{false};
};

template <typename F>
auto ClientExecutor::Submit(F&& fn) -> std::future<std::invoke_result_t<F>> {
  using R = std::invoke_result_t<F>;
  auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
  auto fut  = task->get_future();
  {
    std::scoped_lock lock(mu_);
    if (stop_) {
      throw std::runtime_error("ClientExecutor stopped");
    }
    tasks_.emplace([task]() { (*task)(); });
  }
  cv_.notify_one();
  return fut;
}

}  // namespace us3_turbo_access::client
