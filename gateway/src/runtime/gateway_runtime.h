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
class MetadataService;
class SessionAppService;
class SessionOpener;
class SessionStore;
class SessionSweeper;
}  // namespace us3_turbo_access::gateway::core

namespace us3_turbo_access::gateway::core::multipart {
class MultipartStore;
class MultipartCoordinator;
class MultipartAppService;
}  // namespace us3_turbo_access::gateway::core::multipart

namespace us3_turbo_access::gateway::data_flow::gds {
class GdsExecutor;
class GdsMultipartPathHandler;
}  // namespace us3_turbo_access::gateway::data_flow::gds

namespace us3_turbo_access::gateway::data_flow::ucx {
class UcxExecutor;
class UcxMultipartPathHandler;
}  // namespace us3_turbo_access::gateway::data_flow::ucx

namespace us3_turbo_access::gateway::runtime {
class IoWorkerPool;
}  // namespace us3_turbo_access::gateway::runtime

namespace us3_turbo_access::gateway::api {
class ControlPlaneService;
class RdmaDataPlaneService;
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
  GatewayOptions                                       options_;
  std::shared_ptr<spdlog::logger>                      logger_;
  std::unique_ptr<backend::IBackend>                   backend_;
  std::unique_ptr<core::SessionStore>                  sessions_;
  std::unique_ptr<core::SessionAppService>             session_app_;
  std::unique_ptr<core::SessionSweeper>                session_sweeper_;
  std::unique_ptr<core::multipart::MultipartStore>     multipart_store_;
  std::unique_ptr<core::multipart::MultipartCoordinator> multipart_coordinator_;
  std::unique_ptr<core::multipart::MultipartAppService>  multipart_app_;
  std::unique_ptr<runtime::IoWorkerPool>               io_pool_;
  std::unique_ptr<core::MetadataService>               metadata_;
  std::unique_ptr<data_flow::gds::GdsExecutor>               gds_executor_;
  std::unique_ptr<data_flow::gds::GdsMultipartPathHandler>   gds_multipart_handler_;
  std::unique_ptr<data_flow::ucx::UcxExecutor>               ucx_executor_;
  std::unique_ptr<data_flow::ucx::UcxMultipartPathHandler>   ucx_multipart_handler_;
  std::unique_ptr<core::SessionOpener>                       session_opener_;
  std::unique_ptr<api::ControlPlaneService>            control_plane_;
  std::unique_ptr<api::RdmaDataPlaneService>           rdma_data_plane_;
  brpc::Server                                         server_;
  bool                                                 started_{false};
};

}  // namespace us3_turbo_access::gateway::runtime
