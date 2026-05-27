#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/logger.h>

namespace us3_turbo_access::gateway::data_path::rdma {

class RdmaSessionRegistry;

/**
 * RDMA session 后台清理线程。骨架与 core::SessionSweeper 一致：单 std::thread
 * + condition_variable::wait_for(sweep_interval_)，每轮调 registry.CollectExpired
 * 拿出过期 session_id 列表，再用调用方注入的 erase_fn 逐个释放（要让 sweeper
 * 通过 RdmaExecutor 间接调 Erase + DetachSession，所以这里只接 callback）。
 *
 * sweep_interval / ttl 都由 RdmaOptions 配置；ttl<=0 时 RdmaExecutor 不应启
 * sweeper。
 */
class RdmaSessionSweeper {
 public:
  using EraseFn = std::function<void(const std::string&)>;

  RdmaSessionSweeper(RdmaSessionRegistry& registry,
                     std::chrono::seconds sweep_interval,
                     EraseFn erase_fn,
                     std::shared_ptr<spdlog::logger> logger);
  ~RdmaSessionSweeper();

  RdmaSessionSweeper(const RdmaSessionSweeper&) = delete;
  RdmaSessionSweeper& operator=(const RdmaSessionSweeper&) = delete;

  void Start();
  void Stop();

 private:
  void Run();

  RdmaSessionRegistry&             registry_;
  std::chrono::seconds             sweep_interval_;
  EraseFn                          erase_fn_;
  std::shared_ptr<spdlog::logger>  logger_;

  std::atomic<bool>                running_{false};
  std::mutex                       mu_;
  std::condition_variable          cv_;
  std::thread                      thread_;
};

}  // namespace us3_turbo_access::gateway::data_path::rdma
