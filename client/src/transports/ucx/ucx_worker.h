#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include <ucp/api/ucp.h>

#include "client/src/transports/ucx/ucx_context.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

/**
 * UCP worker 的 RAII 封装，内含独立 progress thread。
 *
 * progress thread 在 Create() 时启动，在析构时安全停止：
 *   1. 设 stop_ flag
 *   2. signal worker (ucp_worker_signal)，让 blocking wait 退出
 *   3. join thread
 *   4. ucp_worker_destroy
 *
 * 线程模式：UCS_THREAD_MODE_MULTI，允许外部线程并发调用 UCX API
 * （PutAsync 从 RdmaTransferPath 的 worker thread 发起）。
 */
class UcxWorker {
 public:
  [[nodiscard]] static Result<std::unique_ptr<UcxWorker>> Create(UcxContext& ctx);

  ~UcxWorker();

  UcxWorker(const UcxWorker&) = delete;
  UcxWorker& operator=(const UcxWorker&) = delete;

  [[nodiscard]] ucp_worker_h handle() const noexcept { return worker_; }

 private:
  UcxWorker() = default;

  void ProgressLoop();

  ucp_worker_h       worker_{nullptr};
  std::atomic<bool>  stop_{false};
  std::thread        progress_thread_;
};

}  // namespace us3_turbo_access::client
