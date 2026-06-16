#pragma once

#include <memory>
#include <string>

#include <brpc/controller.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "control_plane.pb.h"

namespace spdlog {
class logger;
}

namespace us3_turbo_access::gateway::core {
class MetadataService;
class SessionOpener;
class SessionStore;
}  // namespace us3_turbo_access::gateway::core

namespace us3_turbo_access::gateway::core::multipart {
class MultipartAppService;
class MultipartCoordinator;
}  // namespace us3_turbo_access::gateway::core::multipart

namespace us3_turbo_access::gateway::data_path::gds {
class GdsExecutor;
}  // namespace us3_turbo_access::gateway::data_path::gds

namespace us3_turbo_access::gateway::runtime {
class IoWorkerPool;
}  // namespace us3_turbo_access::gateway::runtime

namespace us3_turbo_access::gateway::api {

/**
 * @brief brpc adapter that wires the gateway control-plane proto onto the
 *        core session_opener and the GDS executor.
 *
 * This class only translates between protobuf messages (package
 * `us3_turbo_access.gateway`) and core types — all session / data-path
 * business logic lives in @ref core::SessionOpener and
 * @ref data_path::gds::GdsExecutor.
 */
class ControlPlaneService final : public ::us3_turbo_access::gateway::ControlPlaneService {
 public:
  ControlPlaneService(core::SessionStore& sessions,
                      core::MetadataService& metadata,
                      core::SessionOpener& session_opener,
                      data_path::gds::GdsExecutor* gds_executor,
                      core::multipart::MultipartAppService& multipart_app,
                      core::multipart::MultipartCoordinator& multipart_coord,
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
  // OpenSession/GdsGet/GdsPut 的入口把闭包提交进 io_pool；这里是真正干活的函数。
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

  core::SessionStore&              sessions_;
  core::MetadataService&           metadata_;
  core::SessionOpener&             session_opener_;
  data_path::gds::GdsExecutor*     gds_executor_{nullptr};
  core::multipart::MultipartAppService&   multipart_app_;
  core::multipart::MultipartCoordinator&  multipart_coord_;
  runtime::IoWorkerPool&           io_pool_;
  std::shared_ptr<spdlog::logger>  logger_;
};

}  // namespace us3_turbo_access::gateway::api
