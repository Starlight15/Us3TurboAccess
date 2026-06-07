#pragma once

#include <memory>

#include <brpc/controller.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "rdma_data_plane.pb.h"

namespace spdlog { class logger; }

namespace us3_turbo_access::gateway::data_path::ucx {
class UcxExecutor;
}

namespace us3_turbo_access::gateway::api {

class RdmaDataPlaneService final
    : public ::us3_turbo_access::gateway::RdmaDataPlaneService {
 public:
  explicit RdmaDataPlaneService(data_path::ucx::UcxExecutor& executor,
                                std::shared_ptr<spdlog::logger> logger);

  void DiscoverRdmaEndpoint(
      google::protobuf::RpcController*,
      const ::us3_turbo_access::gateway::RdmaDiscoverRequest*,
      ::us3_turbo_access::gateway::RdmaDiscoverResponse*,
      google::protobuf::Closure*) override;

  void BindSessionToConnection(
      google::protobuf::RpcController*,
      const ::us3_turbo_access::gateway::RdmaBindRequest*,
      ::us3_turbo_access::gateway::RdmaBindResponse*,
      google::protobuf::Closure*) override;

  void CommitObject(
      google::protobuf::RpcController*,
      const ::us3_turbo_access::gateway::RdmaCommitRequest*,
      ::us3_turbo_access::gateway::RdmaCommitResponse*,
      google::protobuf::Closure*) override;

  void CommitPart(
      google::protobuf::RpcController*,
      const ::us3_turbo_access::gateway::RdmaCommitPartRequest*,
      ::us3_turbo_access::gateway::RdmaCommitPartResponse*,
      google::protobuf::Closure*) override;

  void AbortSession(
      google::protobuf::RpcController*,
      const ::us3_turbo_access::gateway::RdmaAbortRequest*,
      ::us3_turbo_access::gateway::RdmaAbortResponse*,
      google::protobuf::Closure*) override;

 private:
  data_path::ucx::UcxExecutor&    executor_;
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace us3_turbo_access::gateway::api
