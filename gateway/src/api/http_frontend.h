#pragma once

#include <memory>
#include <string>

#include <brpc/controller.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "http_gateway.pb.h"

namespace spdlog {
class logger;
}

namespace us3_turbo_access::gateway::core {
class SessionStore;
class TransferEngine;
}  // namespace us3_turbo_access::gateway::core

namespace us3_turbo_access::gateway::api {

/**
 * @brief brpc `http_master_service` that exposes the gateway's HTTP API.
 *
 * Routes:
 *   - `HEAD /v1/objects/{bucket}/{key:*}`  -> object metadata
 *   - `GET  /v1/objects/{bucket}/{key:*}`  -> object body (supports Range)
 *   - `PUT  /v1/objects/{bucket}/{key:*}`  -> upload body
 *   - `GET  /healthz`                       -> liveness probe
 *
 * The same handlers also accept the unversioned `/objects/...` form to ease
 * client migration.
 */
class HttpFrontend final : public ::fusion_access::gateway::GatewayHttpService {
 public:
  HttpFrontend(std::string gateway_id, core::SessionStore& sessions,
               core::TransferEngine& transfers,
               std::shared_ptr<spdlog::logger> logger);

  void default_method(google::protobuf::RpcController* cntl,
                      const ::fusion_access::gateway::GatewayHttpRequest* request,
                      ::fusion_access::gateway::GatewayHttpResponse* response,
                      google::protobuf::Closure* done) override;

 private:
  void HandleHealth(brpc::Controller* cntl);
  void HandleHead(brpc::Controller* cntl, const std::string& bucket,
                  const std::string& key);
  void HandleGet(brpc::Controller* cntl, const std::string& bucket,
                 const std::string& key);
  void HandlePut(brpc::Controller* cntl, const std::string& bucket,
                 const std::string& key);

  std::string                     gateway_id_;
  core::SessionStore&             sessions_;
  core::TransferEngine&           transfers_;
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace us3_turbo_access::gateway::api
