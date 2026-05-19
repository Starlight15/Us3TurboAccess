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
class Negotiator;
class SessionStore;
class TransferEngine;
}  // namespace us3_turbo_access::gateway::core

namespace us3_turbo_access::gateway::data_path::gds {
class GdsExecutor;
}  // namespace us3_turbo_access::gateway::data_path::gds

namespace us3_turbo_access::gateway::api {

/**
 * @brief brpc adapter that wires the gateway control-plane proto onto the
 *        core negotiator and the GDS executor.
 *
 * The proto package is `fusion_access.gateway` for wire compatibility with the
 * existing client SDK; this class only translates between protobuf messages
 * and core types — all session/path business logic lives in
 * @ref core::Negotiator and @ref data_path::gds::GdsExecutor.
 */
class ControlPlaneService final : public ::fusion_access::gateway::ControlPlaneService {
 public:
  ControlPlaneService(core::SessionStore& sessions,
                      core::TransferEngine& transfers,
                      core::Negotiator& negotiator,
                      data_path::gds::GdsExecutor* gds_executor,
                      std::shared_ptr<spdlog::logger> logger);

  void NegotiateTransferSession(
      google::protobuf::RpcController* cntl,
      const ::fusion_access::gateway::NegotiateTransferSessionRequest* request,
      ::fusion_access::gateway::NegotiateTransferSessionResponse* response,
      google::protobuf::Closure* done) override;

  void HeadObject(google::protobuf::RpcController* cntl,
                  const ::fusion_access::gateway::HeadObjectRequest* request,
                  ::fusion_access::gateway::HeadObjectResponse* response,
                  google::protobuf::Closure* done) override;

  void ExecuteGdsGet(
      google::protobuf::RpcController* cntl,
      const ::fusion_access::gateway::ExecuteGdsChunkRequest* request,
      ::fusion_access::gateway::ExecuteGdsChunkResponse* response,
      google::protobuf::Closure* done) override;

  void ExecuteGdsPut(
      google::protobuf::RpcController* cntl,
      const ::fusion_access::gateway::ExecuteGdsChunkRequest* request,
      ::fusion_access::gateway::ExecuteGdsChunkResponse* response,
      google::protobuf::Closure* done) override;

 private:
  core::SessionStore&              sessions_;
  core::TransferEngine&            transfers_;
  core::Negotiator&                negotiator_;
  data_path::gds::GdsExecutor*     gds_executor_{nullptr};
  std::shared_ptr<spdlog::logger>  logger_;
};

}  // namespace us3_turbo_access::gateway::api
