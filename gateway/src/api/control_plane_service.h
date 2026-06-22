#pragma once

#include <memory>
#include <string>

#include <brpc/controller.h>
#include <bvar/bvar.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "control_plane.pb.h"
#include "us3_turbo_access/gateway/result.h"

namespace spdlog {
class logger;
}

namespace us3_turbo_access::gateway::core {
class MetadataService;
class SessionAppService;
class SessionOpener;
struct Session;
}  // namespace us3_turbo_access::gateway::core

namespace us3_turbo_access::gateway::core::multipart {
class MultipartAppService;
}  // namespace us3_turbo_access::gateway::core::multipart

namespace us3_turbo_access::gateway::data_flow::gds {
class GdsExecutor;
class GdsMultipartPathHandler;
}  // namespace us3_turbo_access::gateway::data_flow::gds

namespace us3_turbo_access::gateway::runtime {
class IoWorkerPool;
}  // namespace us3_turbo_access::gateway::runtime

namespace us3_turbo_access::gateway::api {

/**
 * @brief brpc adapter that wires the gateway control-plane proto onto
 *        application services and data-path executors.
 *
 * This class only translates between protobuf messages (package
 * `us3_turbo_access.gateway`) and core types — all session lifecycle,
 * multipart orchestration, and data-path logic lives in dedicated
 * service/handler classes:
 *
 *   - Session lifecycle    → @ref core::SessionAppService
 *   - Multipart control    → @ref core::multipart::MultipartAppService
 *   - GDS multipart part   → @ref data_flow::gds::GdsMultipartPathHandler
 *   - Session opening      → @ref core::SessionOpener
 *   - GDS data path        → @ref data_flow::gds::GdsExecutor
 */
class ControlPlaneService final : public ::us3_turbo_access::gateway::ControlPlaneService {
 public:
  ControlPlaneService(core::SessionAppService& session_app,
                      core::MetadataService& metadata,
                      core::SessionOpener& session_opener,
                      data_flow::gds::GdsExecutor* gds_executor,
                      data_flow::gds::GdsMultipartPathHandler* gds_multipart_handler,
                      core::multipart::MultipartAppService& multipart_app,
                      runtime::IoWorkerPool& io_pool,
                      std::shared_ptr<spdlog::logger> logger);

  void OpenSession(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo_access::gateway::OpenSessionRequest* request,
      ::us3_turbo_access::gateway::OpenSessionResponse* response,
      google::protobuf::Closure* done) override;

  void HeadObject(google::protobuf::RpcController* cntl,
                  const ::us3_turbo_access::gateway::HeadObjectRequest* request,
                  ::us3_turbo_access::gateway::HeadObjectResponse* response,
                  google::protobuf::Closure* done) override;

  void GdsGet(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo_access::gateway::GdsChunkRequest* request,
      ::us3_turbo_access::gateway::GdsChunkResponse* response,
      google::protobuf::Closure* done) override;

  void GdsPut(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo_access::gateway::GdsChunkRequest* request,
      ::us3_turbo_access::gateway::GdsChunkResponse* response,
      google::protobuf::Closure* done) override;

  void StartUpload(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo_access::gateway::StartUploadRequest* request,
      ::us3_turbo_access::gateway::StartUploadResponse* response,
      google::protobuf::Closure* done) override;

  void CompleteUpload(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo_access::gateway::CompleteUploadRequest* request,
      ::us3_turbo_access::gateway::CompleteUploadResponse* response,
      google::protobuf::Closure* done) override;

  void AbortUpload(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo_access::gateway::AbortUploadRequest* request,
      ::us3_turbo_access::gateway::AbortUploadResponse* response,
      google::protobuf::Closure* done) override;

  void AbortSession(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo_access::gateway::AbortSessionRequest* request,
      ::us3_turbo_access::gateway::AbortSessionResponse* response,
      google::protobuf::Closure* done) override;

 private:
  // 在 io_pool worker 线程上执行的 RPC handler 实现。
  void HandleOpenSession(
      brpc::Controller* cntl,
      const ::us3_turbo_access::gateway::OpenSessionRequest* request,
      ::us3_turbo_access::gateway::OpenSessionResponse* response,
      google::protobuf::Closure* done);

  void HandleGdsGet(
      brpc::Controller* cntl,
      const ::us3_turbo_access::gateway::GdsChunkRequest* request,
      ::us3_turbo_access::gateway::GdsChunkResponse* response,
      google::protobuf::Closure* done);

  void HandleGdsPut(
      brpc::Controller* cntl,
      const ::us3_turbo_access::gateway::GdsChunkRequest* request,
      ::us3_turbo_access::gateway::GdsChunkResponse* response,
      google::protobuf::Closure* done);

  /// Common pre-checks for GDS chunk handlers: executor availability,
  /// session resolution, rdma_token, BumpActive.  On failure, records
  /// fail_metric and calls cntl->SetFailed().
  Result<std::shared_ptr<core::Session>> PrepareGdsChunk(
      brpc::Controller* cntl,
      const ::us3_turbo_access::gateway::GdsChunkRequest* request,
      bvar::Adder<std::int64_t>& fail_metric);

  core::SessionAppService&                          session_app_;
  core::MetadataService&                            metadata_;
  core::SessionOpener&                              session_opener_;
  data_flow::gds::GdsExecutor*                      gds_executor_{nullptr};
  data_flow::gds::GdsMultipartPathHandler*          gds_multipart_handler_{nullptr};
  core::multipart::MultipartAppService&             multipart_app_;
  runtime::IoWorkerPool&                            io_pool_;
  std::shared_ptr<spdlog::logger>                   logger_;
};

}  // namespace us3_turbo_access::gateway::api
