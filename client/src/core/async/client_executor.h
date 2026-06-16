#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <bthread/bthread.h>

namespace us3_turbo_access::client {

/**
 * 客户端共享任务调度器：每次 Submit 直接拉起一个 brpc bthread 执行任务，
 * 与 brpc 网络栈共享调度，避免独立线程池的 mutex 争用 + 线程切换开销。
 *
 * 与 brpc bthread 调度一致：bthread 是 M:N 协程，能在 ~固定数 worker 线程
 * （brpc 默认 = CPU 数）上跑成千上万的轻量任务，无需自管 worker pool。
 *
 * 公开 Submit 签名保持不变：返回 std::future，调用者按值或按引用阻塞等待。
 *
 * worker_threads 参数仅用于兼容老 API：实际并发由 brpc 全局 bthread worker
 * pool 决定（受 brpc 自带 gflag --bthread_concurrency 控制）。
 *
 * 注意：任务函数内若调 std::this_thread::sleep 等阻塞 syscall，仍会占住
 * 一个 worker pthread，并非真协程让出。这一点和原 std::thread 池一致。
 */
class ClientExecutor {
 public:
  explicit ClientExecutor(std::size_t worker_threads);
  ~ClientExecutor();

  ClientExecutor(const ClientExecutor&) = delete;
  ClientExecutor& operator=(const ClientExecutor&) = delete;

  /** Submit 后立即返回 future；任务在某个 bthread 上执行。Stop 后再调抛。*/
  template <typename F>
  [[nodiscard]] auto Submit(F&& fn) -> std::future<std::invoke_result_t<F>>;

  /**
   * 显式排空 + 关闭。返回 true 表示 grace 期内所有 inflight 都退出。
   * 返回 false 表示 grace 超时仍有 inflight 任务：这种情况下任务可能在
   * ClientExecutor 销毁后访问 inflight_ → UB；调用方应记 unclean 退出。
   *
   * 用法：
   *   ClientCore::Shutdown 显式调 executor.Shutdown(default_grace=5s)
   *   ~ClientExecutor 析构兜底（也调，幂等），并在 false 时 log warning
   */
  bool Shutdown(std::chrono::milliseconds grace);

  /** 累计的"不洁退出"次数（grace 超时 + 仍有 inflight）。调试用。 */
  [[nodiscard]] std::int64_t unclean_shutdowns() const noexcept {
    return unclean_shutdowns_.load(std::memory_order_acquire);
  }

 private:
  // 每个 Submit 任务的 trampoline 入口，bthread_start_background 调用。
  static void* TaskEntry(void* arg);

  std::atomic<bool>             stop_{false};
  std::atomic<std::int64_t>     inflight_{0};
  std::atomic<std::int64_t>     unclean_shutdowns_{0};
};

template <typename F>
auto ClientExecutor::Submit(F&& fn) -> std::future<std::invoke_result_t<F>> {
  using R = std::invoke_result_t<F>;
  if (stop_.load(std::memory_order_acquire)) {
    throw std::runtime_error("ClientExecutor stopped");
  }
  auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
  auto fut  = task->get_future();

  inflight_.fetch_add(1, std::memory_order_acq_rel);

  // 把"运行 task + 减 inflight"打包成 std::function<void()>，堆分配后
  // 通过裸指针交给 bthread，由 TaskEntry 在 bthread 上下文里 delete。
  auto* boxed = new std::function<void()>(
      [task, this]() {
        (*task)();
        inflight_.fetch_sub(1, std::memory_order_acq_rel);
      });

  bthread_t tid;
  // BTHREAD_ATTR_NORMAL：与 brpc worker 共享 pthread 池，不绑核。
  if (bthread_start_background(&tid, &BTHREAD_ATTR_NORMAL, &TaskEntry, boxed) != 0) {
    // 启动失败：inline 跑一遍并直接销毁 boxed，让 future 仍然完成。
    delete boxed;
    (*task)();
    inflight_.fetch_sub(1, std::memory_order_acq_rel);
  }
  return fut;
}

}  // namespace us3_turbo_access::client
