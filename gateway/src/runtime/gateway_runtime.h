#pragma once

#include <memory>

#include <brpc/server.h>

#include "us3_turbo_access/gateway/options.h"
#include "us3_turbo_access/gateway/result.h"

namespace spdlog {
class logger;
}

namespace us3_turbo_access::gateway::backend {
class IBackend;
}

namespace us3_turbo_access::gateway::core {
class Negotiator;
class SessionStore;
class TransferEngine;
}  // namespace us3_turbo_access::gateway::core

namespace us3_turbo_access::gateway::data_path::gds {
class GdsExecutor;
}  // namespace us3_turbo_access::gateway::data_path::gds

namespace us3_turbo_access::gateway::api {
class ControlPlaneService;
class HttpFrontend;
}  // namespace us3_turbo_access::gateway::api

namespace us3_turbo_access::gateway::runtime {

/**
 * @brief Assembles and owns every runtime component of a gateway instance.
 *
 * Lifecycle (mirroring `client::ClientCore`):
 *   1. Construct with options (no side effects).
 *   2. Call Initialize() to build dependencies and bind the brpc server.
 *   3. Call Run() to block on `brpc::Server::RunUntilAskedToQuit()`.
 *   4. Call Shutdown() exactly once to release everything.
 */
class GatewayRuntime {
 public:
  explicit GatewayRuntime(GatewayOptions options);
  ~GatewayRuntime();

  GatewayRuntime(const GatewayRuntime&) = delete;
  GatewayRuntime& operator=(const GatewayRuntime&) = delete;
  GatewayRuntime(GatewayRuntime&&) = delete;
  GatewayRuntime& operator=(GatewayRuntime&&) = delete;

  [[nodiscard]] Result<bool> Initialize();
  void Shutdown();
  void Run();

  [[nodiscard]] bool                    initialized() const noexcept;
  [[nodiscard]] const GatewayOptions&   options() const noexcept;

 private:
  GatewayOptions                                   options_;
  std::shared_ptr<spdlog::logger>                  logger_;
  std::unique_ptr<backend::IBackend>               backend_;
  std::unique_ptr<core::SessionStore>              sessions_;
  std::unique_ptr<core::TransferEngine>            transfers_;
  std::unique_ptr<core::Negotiator>                negotiator_;
  std::unique_ptr<data_path::gds::GdsExecutor>     gds_executor_;
  std::unique_ptr<api::ControlPlaneService>        control_plane_;
  std::unique_ptr<api::HttpFrontend>               http_frontend_;
  brpc::Server                                     server_;
  bool                                             started_{false};
};

}  // namespace us3_turbo_access::gateway::runtime
