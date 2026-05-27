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
class MetadataService;
}  // namespace us3_turbo_access::gateway::core

namespace us3_turbo_access::gateway::core::multipart {
class MultipartCoordinator;
}  // namespace us3_turbo_access::gateway::core::multipart

namespace us3_turbo_access::gateway::data_path::http {
class HttpExecutor;
}  // namespace us3_turbo_access::gateway::data_path::http

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
class HttpFrontend final : public ::us3_turbo_access::gateway::GatewayHttpService {
 public:
  HttpFrontend(std::string gateway_id, core::MetadataService& metadata,
               data_path::http::HttpExecutor& http,
               core::multipart::MultipartCoordinator& multipart,
               std::shared_ptr<spdlog::logger> logger);

  void default_method(google::protobuf::RpcController* cntl,
                      const ::us3_turbo_access::gateway::GatewayHttpRequest* request,
                      ::us3_turbo_access::gateway::GatewayHttpResponse* response,
                      google::protobuf::Closure* done) override;

 private:
  void HandleHealth(brpc::Controller* cntl);
  void HandleVars(brpc::Controller* cntl, const std::string& path);
  void HandleHead(brpc::Controller* cntl, const std::string& bucket,
                  const std::string& key);
  void HandleGet(brpc::Controller* cntl, const std::string& bucket,
                 const std::string& key);
  void HandlePut(brpc::Controller* cntl, const std::string& bucket,
                 const std::string& key);

  // V2 multipart 路由（/v1/uploads/...）
  void HandleStartUpload(brpc::Controller* cntl, const std::string& bucket,
                         const std::string& key);
  void HandleUploadPart(brpc::Controller* cntl, const std::string& upload_id,
                        std::uint32_t part_number);
  void HandleCompleteUpload(brpc::Controller* cntl,
                            const std::string& upload_id);
  void HandleAbortUpload(brpc::Controller* cntl,
                         const std::string& upload_id);

  std::string                              gateway_id_;
  core::MetadataService&                   metadata_;
  data_path::http::HttpExecutor&           http_;
  core::multipart::MultipartCoordinator&   multipart_;
  std::shared_ptr<spdlog::logger>          logger_;
};

}  // namespace us3_turbo_access::gateway::api
