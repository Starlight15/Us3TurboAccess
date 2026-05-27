#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <spdlog/logger.h>

#include "us3_turbo_access/gateway/options.h"
#include "us3_turbo_access/gateway/result.h"

struct rdma_event_channel;
struct rdma_cm_id;

namespace us3_turbo_access::gateway::runtime {
class IoWorkerPool;
}

namespace us3_turbo_access::gateway::data_path::rdma {

class RdmaResources;
class RdmaConnectionRegistry;

/**
 * 服务端 RDMA listener：bind + listen + 一个后台线程跑 rdma_cm event loop。
 *
 * CONNECT_REQUEST：派发到 io_pool（避免 event loop 串行化 PD/QP 创建）。
 * Worker 在 cm_id->verbs 上建 PD/CQ/QP（无 session 绑定），分配 conn_token
 * 注册到 RdmaConnectionRegistry，通过 ACCEPT private_data 回传。
 * DISCONNECTED：先 rdma_disconnect 完成对端 ack，再 EraseByCmId 释放资源。
 *
 * session 级的 buffer/MR 不在这里碰；BindSessionToConnection RPC 来分配。
 */
class RdmaListener {
 public:
  RdmaListener(std::shared_ptr<RdmaResources> resources,
               RdmaConnectionRegistry* connection_registry,
               const RdmaOptions& opts,
               runtime::IoWorkerPool* io_pool,
               std::shared_ptr<spdlog::logger> logger);
  ~RdmaListener();

  RdmaListener(const RdmaListener&) = delete;
  RdmaListener& operator=(const RdmaListener&) = delete;

  [[nodiscard]] Result<bool> Start(const std::string& bind_host);
  void                       Stop();

  [[nodiscard]] bool         running() const noexcept {
    return running_.load(std::memory_order_acquire);
  }
  [[nodiscard]] int          port() const noexcept { return opts_.listen_port; }

 private:
  void EventLoop();
  void HandleConnectRequest(rdma_cm_id* id);

  std::shared_ptr<RdmaResources>       resources_;
  RdmaConnectionRegistry*              connection_registry_{nullptr};
  RdmaOptions                          opts_;
  runtime::IoWorkerPool*               io_pool_{nullptr};
  std::shared_ptr<spdlog::logger>      logger_;

  rdma_event_channel*                  event_channel_{nullptr};
  rdma_cm_id*                          listen_id_{nullptr};

  std::atomic<bool>                    running_{false};
  std::thread                          event_thread_;
};

}  // namespace us3_turbo_access::gateway::data_path::rdma
