#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <spdlog/logger.h>

namespace us3_turbo_access::gateway::core {

class SessionStore;
}  // namespace us3_turbo_access::gateway::core

namespace us3_turbo_access::gateway::core::multipart {
class MultipartCoordinator;
class MultipartStore;
}  // namespace us3_turbo_access::gateway::core::multipart

namespace us3_turbo_access::gateway::core {

class SessionSweeper {
 public:
  SessionSweeper(SessionStore& sessions, std::chrono::seconds sweep_interval,
                 std::shared_ptr<spdlog::logger> logger);

  void SetMultipart(multipart::MultipartStore* store,
                    multipart::MultipartCoordinator* coordinator);
  ~SessionSweeper();

  SessionSweeper(const SessionSweeper&) = delete;
  SessionSweeper& operator=(const SessionSweeper&) = delete;

  void Start();
  void Stop();

 private:
  void Run();

  SessionStore&                    sessions_;
  std::chrono::seconds             sweep_interval_;
  std::shared_ptr<spdlog::logger>  logger_;
  multipart::MultipartStore*       multipart_store_{nullptr};
  multipart::MultipartCoordinator* multipart_coordinator_{nullptr};
  std::atomic<bool>                running_{false};
  std::mutex                       mu_;
  std::condition_variable          cv_;
  std::thread                      thread_;
};

}  // namespace us3_turbo_access::gateway::core
