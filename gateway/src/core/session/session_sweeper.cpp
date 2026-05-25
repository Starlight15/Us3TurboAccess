#include "core/session/session_sweeper.h"

#include <utility>
#include <vector>

#include "common/metrics.h"
#include "core/multipart/multipart_coordinator.h"
#include "core/multipart/multipart_store.h"
#include "core/session/session_store.h"

namespace us3_turbo_access::gateway::core {

SessionSweeper::SessionSweeper(SessionStore& sessions,
                               std::chrono::seconds sweep_interval,
                               std::shared_ptr<spdlog::logger> logger)
    : sessions_(sessions),
      sweep_interval_(sweep_interval),
      logger_(std::move(logger)) {}

SessionSweeper::~SessionSweeper() { Stop(); }

void SessionSweeper::SetMultipart(multipart::MultipartStore* store,
                                  multipart::MultipartCoordinator* coordinator) {
  multipart_store_ = store;
  multipart_coordinator_ = coordinator;
}

void SessionSweeper::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }
  thread_ = std::thread(&SessionSweeper::Run, this);
}

void SessionSweeper::Stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) {
    return;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

// 定时扫过期 session 与 multipart upload；Stop() 通过 notify_all 立即唤醒。
void SessionSweeper::Run() {
  while (running_.load(std::memory_order_acquire)) {
    {
      std::unique_lock lock(mu_);
      const auto wait_status = cv_.wait_for(lock, sweep_interval_);
      (void)wait_status;  // timeout / 唤醒都走下方 running_ 检查
    }
    if (!running_.load(std::memory_order_acquire)) {
      break;
    }

    const auto evicted = sessions_.SweepExpired(std::chrono::steady_clock::now());
    if (evicted != 0) {
      common::metrics().sessions_evicted_total << static_cast<std::int64_t>(evicted);
      if (logger_ != nullptr) {
        logger_->info("session sweeper evicted {} expired sessions", evicted);
      }
    }
    // store 已移走过期 upload；这里只需触发 backend 侧 abort。
    if (multipart_store_ != nullptr && multipart_coordinator_ != nullptr) {
      std::vector<std::shared_ptr<multipart::MultipartUpload>> expired;
      const auto removed = multipart_store_->SweepExpired(
          std::chrono::steady_clock::now(), expired);
      for (const auto& upload : expired) {
        std::string backend_id;
        {
          std::scoped_lock ulock(upload->mu);
          backend_id = upload->backend_upload_id;
          upload->state = multipart::State::kAborted;
        }
        multipart_coordinator_->AbortBackend(backend_id);
      }
      if (removed != 0) {
        common::metrics().multipart_swept_total << static_cast<std::int64_t>(removed);
        if (logger_ != nullptr) {
          logger_->info("multipart sweeper evicted {} expired uploads", removed);
        }
      }
    }
  }
}

}  // namespace us3_turbo_access::gateway::core
