#include "api/control_plane_service.h"

#include <utility>

#include <brpc/closure_guard.h>
#include <spdlog/logger.h>

#include "core/negotiator/negotiator.h"
#include "core/session/session_store.h"
#include "core/transfer/transfer_engine.h"
#include "data_path/gds/gds_executor.h"
#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway::api {

namespace {

void FillChunkPlan(const core::Session& session,
                   ::fusion_access::gateway::NegotiateTransferSessionResponse* resp) {
  for (const auto& item : session.chunk_plan) {
    auto* chunk = resp->add_chunk_plan();
    chunk->set_offset(item.offset);
    chunk->set_size(item.size);
  }
}

void FillRdmaParameters(
    const core::RdmaParameters& params,
    ::fusion_access::gateway::NegotiateTransferSessionResponse* resp) {
  auto* out = resp->mutable_rdma_parameters();
  for (const auto& [k, v] : params.ToMap()) {
    (*out)[k] = v;
  }
}

[[nodiscard]] std::shared_ptr<core::Session> ResolveSession(
    core::SessionStore& sessions,
    const ::fusion_access::gateway::ExecuteGdsChunkRequest& request) {
  if (!request.session_id().empty()) {
    auto lookup = sessions.Find(request.session_id());
    if (lookup.success()) {
      return lookup.value();
    }
  }
  if (!request.transfer_ticket().empty()) {
    auto lookup = sessions.FindByTicket(request.transfer_ticket());
    if (lookup.success()) {
      return lookup.value();
    }
  }
  return nullptr;
}

void FillGdsResponse(const std::string& gateway_id,
                     const std::string& transfer_status,
                     const std::string& rdma_reply, const std::string& etag,
                     const std::string& version,
                     ::fusion_access::gateway::ExecuteGdsChunkResponse* resp) {
  resp->set_selected_gateway(gateway_id);
  resp->set_gateway_id(gateway_id);
  resp->set_transfer_status(transfer_status);
  resp->set_rdma_reply(rdma_reply);
  resp->set_etag(etag);
  resp->set_version(version);
}

[[nodiscard]] core::NegotiateRequest ToNegotiateRequest(
    const ::fusion_access::gateway::NegotiateTransferSessionRequest& request) {
  core::NegotiateRequest req;
  req.session_id = request.session_id();
  req.request_id = request.request_id();
  req.bucket = request.bucket();
  req.object_key = request.object_key();
  req.op = ParseOperationType(request.op_type());
  req.data_path = ParseDataPath(request.data_path());
  req.buffer_type = request.buffer_type();
  req.channel_id = request.channel_id();
  req.offset = request.offset();
  req.expected_size = request.expected_size();
  req.idempotency_key = request.idempotency_key();
  return req;
}

void FillNegotiateResponse(
    const core::NegotiationOutcome& outcome,
    ::fusion_access::gateway::NegotiateTransferSessionResponse* response) {
  const auto& session = *outcome.session;
  response->set_request_id(session.request_id);
  response->set_session_id(session.session_id);
  response->set_ticket(session.ticket);
  response->set_gateway_id(session.gateway_id);
  response->set_gateway_endpoint(outcome.gateway_endpoint);
  response->set_channel_id(session.channel_id);
  response->set_async_handle(session.async_handle);
  response->set_expire_at(session.expire_at);
  FillChunkPlan(session, response);
  FillRdmaParameters(outcome.rdma_parameters, response);
}

}  // namespace

ControlPlaneService::ControlPlaneService(core::SessionStore& sessions,
                                         core::TransferEngine& transfers,
                                         core::Negotiator& negotiator,
                                         data_path::gds::GdsExecutor* gds_executor,
                                         std::shared_ptr<spdlog::logger> logger)
    : sessions_(sessions),
      transfers_(transfers),
      negotiator_(negotiator),
      gds_executor_(gds_executor),
      logger_(std::move(logger)) {}

void ControlPlaneService::NegotiateTransferSession(
    google::protobuf::RpcController* cntl_base,
    const ::fusion_access::gateway::NegotiateTransferSessionRequest* request,
    ::fusion_access::gateway::NegotiateTransferSessionResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  auto result = negotiator_.Negotiate(ToNegotiateRequest(*request));
  if (!result.success()) {
    cntl->SetFailed(result.error().message);
    return;
  }
  FillNegotiateResponse(result.value(), response);
}

void ControlPlaneService::HeadObject(
    google::protobuf::RpcController* cntl_base,
    const ::fusion_access::gateway::HeadObjectRequest* request,
    ::fusion_access::gateway::HeadObjectResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  auto head = transfers_.Head(request->bucket(), request->object_key());
  if (!head.success()) {
    cntl->SetFailed(head.error().message);
    return;
  }
  response->set_content_length(head.value().content_length);
  response->set_etag(head.value().etag);
  response->set_version(head.value().version);
  (*response->mutable_headers())["Content-Length"] =
      std::to_string(head.value().content_length);
  (*response->mutable_headers())["ETag"] = head.value().etag;
  (*response->mutable_headers())["x-fa-version"] = head.value().version;
}

void ControlPlaneService::ExecuteGdsGet(
    google::protobuf::RpcController* cntl_base,
    const ::fusion_access::gateway::ExecuteGdsChunkRequest* request,
    ::fusion_access::gateway::ExecuteGdsChunkResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  if (gds_executor_ == nullptr || !gds_executor_->available()) {
    cntl->SetFailed("gds-cuobject service is not available on gateway");
    return;
  }
  auto session = ResolveSession(sessions_, *request);
  if (session == nullptr) {
    cntl->SetFailed("gds-cuobject session not found");
    return;
  }
  if (request->rdma_token().empty()) {
    cntl->SetFailed("missing rdma token for gds-cuobject request");
    return;
  }
  auto head = transfers_.Head(request->bucket(), request->object_key());
  if (!head.success()) {
    cntl->SetFailed(head.error().message);
    return;
  }
  auto chunk = gds_executor_->GetChunk(*session, request->rdma_token(),
                                       request->chunk_offset(),
                                       request->chunk_size());
  if (!chunk.success()) {
    cntl->SetFailed(chunk.error().message);
    return;
  }
  FillGdsResponse(session->gateway_id, "completed", chunk.value(),
                  head.value().etag, head.value().version, response);
}

void ControlPlaneService::ExecuteGdsPut(
    google::protobuf::RpcController* cntl_base,
    const ::fusion_access::gateway::ExecuteGdsChunkRequest* request,
    ::fusion_access::gateway::ExecuteGdsChunkResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  if (gds_executor_ == nullptr || !gds_executor_->available()) {
    cntl->SetFailed("gds-cuobject service is not available on gateway");
    return;
  }
  auto session = ResolveSession(sessions_, *request);
  if (session == nullptr) {
    cntl->SetFailed("gds-cuobject session not found");
    return;
  }
  if (request->rdma_token().empty()) {
    cntl->SetFailed("missing rdma token for gds-cuobject request");
    return;
  }
  auto written = gds_executor_->PutChunk(*session, request->rdma_token(),
                                         request->chunk_offset(),
                                         request->chunk_size());
  if (!written.success()) {
    cntl->SetFailed(written.error().message);
    return;
  }
  FillGdsResponse(session->gateway_id, "completed", "gds-cuobject-rdma-read",
                  written.value().etag, written.value().version, response);
}

}  // namespace us3_turbo_access::gateway::api
