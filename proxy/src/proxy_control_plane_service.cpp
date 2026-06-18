#include "proxy/src/proxy_control_plane_service.h"

#include <string>
#include <utility>

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <spdlog/spdlog.h>

namespace us3_turbo_access::proxy {

namespace {

// GDS 通道标识，与 client DataPath::kGdsCuObject → ToString("gds-cuobject") 一致。
constexpr std::string_view kGdsDataPath = "gds-cuobject";
constexpr std::string_view kOpTypePut   = "PUT";

// v1 不实现的方法统一报错信息。
constexpr std::string_view kNotImplemented = "not implemented in proxy v1";

}  // namespace

ProxyControlPlaneService::ProxyControlPlaneService(std::string gateway_id,
                                                   SessionManager& session_mgr)
    : gateway_id_(std::move(gateway_id)), session_mgr_(session_mgr) {}

void ProxyControlPlaneService::OpenSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::OpenSessionRequest* request,
    ::us3_turbo_access::gateway::OpenSessionResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  // ---- 校验 ----
  if (request->data_path() != kGdsDataPath) {
    cntl->SetFailed(brpc::EREQUEST,
                    "proxy v1 only accepts data_path=gds-cuobject, got: %s",
                    request->data_path().c_str());
    return;
  }
  if (request->op_type() != kOpTypePut) {
    cntl->SetFailed(brpc::EREQUEST,
                    "proxy v1 only accepts op_type=PUT, got: %s",
                    request->op_type().c_str());
    return;
  }
  if (request->is_multipart_part()) {
    cntl->SetFailed(brpc::EREQUEST,
                    "proxy v1 does not support multipart part sessions");
    return;
  }

  // ---- 生成 session 凭证（委托给 SessionManager）----
  const auto session = session_mgr_.CreateSession(request->session_id());

  // ---- 填充响应 ----
  response->set_request_id(request->request_id());
  response->set_session_id(session.session_id);
  response->set_ticket(session.ticket);
  response->set_gateway_id(gateway_id_);
  response->set_expire_at(session.expire_at);

  spdlog::info("OpenSession: session_id={}, ticket={}, bucket={}, key={}",
               session.session_id, session.ticket, request->bucket(),
               request->object_key());
}

// ---------------------------------------------------------------------------
// v1 占位方法：一律返回 "not implemented in proxy v1"
// ---------------------------------------------------------------------------

void ProxyControlPlaneService::HeadObject(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::HeadObjectRequest* /*request*/,
    ::us3_turbo_access::gateway::HeadObjectResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

void ProxyControlPlaneService::GdsGet(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::GdsChunkRequest* /*request*/,
    ::us3_turbo_access::gateway::GdsChunkResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

void ProxyControlPlaneService::GdsPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::GdsChunkRequest* /*request*/,
    ::us3_turbo_access::gateway::GdsChunkResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

// 客户端失败路径会调 AbortSession；v1 happy path 不触发，留 SetFailed 占位。
void ProxyControlPlaneService::AbortSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::AbortSessionRequest* /*request*/,
    ::us3_turbo_access::gateway::AbortSessionResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

void ProxyControlPlaneService::StartUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::StartUploadRequest* /*request*/,
    ::us3_turbo_access::gateway::StartUploadResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

void ProxyControlPlaneService::CompleteUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::CompleteUploadRequest* /*request*/,
    ::us3_turbo_access::gateway::CompleteUploadResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

void ProxyControlPlaneService::AbortUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::AbortUploadRequest* /*request*/,
    ::us3_turbo_access::gateway::AbortUploadResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(brpc::ENOMETHOD, "%s", std::string(kNotImplemented).c_str());
}

}  // namespace us3_turbo_access::proxy
