#include "client/src/transports/ucx/ucx_worker.h"

#include <sched.h>

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

Result<std::unique_ptr<UcxWorker>> UcxWorker::Create(UcxContext& ctx) {
  ucp_worker_params_t params{};
  params.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
  params.thread_mode = UCS_THREAD_MODE_MULTI;

  ucp_worker_h worker = nullptr;
  ucs_status_t st = ucp_worker_create(ctx.handle(), &params, &worker);
  if (st != UCS_OK) {
    return Result<std::unique_ptr<UcxWorker>>::Failure(
        MakeRegistrationFailure(ucs_status_string(st)));
  }

  auto obj = std::unique_ptr<UcxWorker>(new UcxWorker());
  obj->worker_ = worker;
  obj->progress_thread_ = std::thread(&UcxWorker::ProgressLoop, obj.get());
  return Result<std::unique_ptr<UcxWorker>>::Success(std::move(obj));
}

UcxWorker::~UcxWorker() {
  if (worker_ == nullptr) return;

  stop_.store(true, std::memory_order_release);
  // 唤醒可能阻塞在 ucp_worker_wait 的线程
  ucp_worker_signal(worker_);
  if (progress_thread_.joinable()) {
    progress_thread_.join();
  }
  ucp_worker_destroy(worker_);
  worker_ = nullptr;
}

void UcxWorker::ProgressLoop() {
  // MULTI mode 不支持 get_efd；用 spin-then-yield 降低延迟
  // 有工作时 spin，连续 N 次无工作后 sched_yield 让出 CPU
  int idle = 0;
  while (!stop_.load(std::memory_order_acquire)) {
    if (ucp_worker_progress(worker_)) {
      idle = 0;
    } else {
      if (++idle > 64) {
        sched_yield();
        idle = 0;
      }
    }
  }
  for (int i = 0; i < 16; ++i) ucp_worker_progress(worker_);
}

}  // namespace us3_turbo_access::client
