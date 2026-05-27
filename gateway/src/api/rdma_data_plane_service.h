#pragma once

#include <memory>

#include <brpc/controller.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "rdma_data_plane.pb.h"

namespace spdlog { class logger; }

namespace us3_turbo_access::gateway::data_path::rdma {
class RdmaExecutor;
}  // namespace us3_turbo_access::gateway::data_path::rdma

namespace us3_turbo_access::gateway::api {

/**
 * brpc 适配层：把 RdmaDataPlaneService proto 接到 RdmaExecutor 上。
 * 控制面与 GDS 完全独立，proto 文件、service 实现、配置参数都分开。
 */
class RdmaDataPlaneService final
    : public ::us3_turbo_access::gateway::RdmaDataPlaneService {
 public:
  explicit RdmaDataPlaneService(data_path::rdma::RdmaExecutor& executor,
                                std::shared_ptr<spdlog::logger> logger);

  void DiscoverRdmaEndpoint(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo_access::gateway::RdmaDiscoverRequest* request,
      ::us3_turbo_access::gateway::RdmaDiscoverResponse* response,
      google::protobuf::Closure* done) override;

  void BindSessionToConnection(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo_access::gateway::RdmaBindRequest* request,
      ::us3_turbo_access::gateway::RdmaBindResponse* response,
      google::protobuf::Closure* done) override;

  void CommitObject(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo_access::gateway::RdmaCommitRequest* request,
      ::us3_turbo_access::gateway::RdmaCommitResponse* response,
      google::protobuf::Closure* done) override;

  void CommitPart(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo_access::gateway::RdmaCommitPartRequest* request,
      ::us3_turbo_access::gateway::RdmaCommitPartResponse* response,
      google::protobuf::Closure* done) override;

  void AbortSession(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo_access::gateway::RdmaAbortRequest* request,
      ::us3_turbo_access::gateway::RdmaAbortResponse* response,
      google::protobuf::Closure* done) override;

 private:
  data_path::rdma::RdmaExecutor&  executor_;
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace us3_turbo_access::gateway::api
