#include "api/control_plane_service.h"

#include <functional>
#include <utility>

#include <brpc/closure_guard.h>
#include <spdlog/logger.h>

#include "common/metrics.h"
#include "backend/backend.h"
#include "core/metadata/metadata_service.h"
#include "core/multipart/multipart_app_service.h"
#include "core/multipart/multipart_coordinator.h"
#include "core/session_opener/session_opener.h"
#include "core/session/session_store.h"
#include "data_path/gds/gds_executor.h"
#include "runtime/io_worker_pool.h"
#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway::api {

namespace {

[[nodiscard]] std::shared_ptr<core::Session> ResolveSession(
    core::SessionStore& sessions,
    const ::us3_turbo_access::gateway::GdsChunkRequest& request) {
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
                     const std::string& version, std::uint32_t crc32c,
                     ::us3_turbo_access::gateway::GdsChunkResponse* resp) {
  resp->set_selected_gateway(gateway_id);
  resp->set_gateway_id(gateway_id);
  resp->set_transfer_status(transfer_status);
  resp->set_rdma_reply(rdma_reply);
  resp->set_etag(etag);
  resp->set_version(version);
  resp->set_crc32c(crc32c);
}

[[nodiscard]] core::OpenSessionParams ToOpenSessionParams(
    const ::us3_turbo_access::gateway::OpenSessionRequest& request) {
  core::OpenSessionParams req;
  req.session_id = request.session_id();
  req.request_id = request.request_id();
  req.bucket = request.bucket();
  req.object_key = request.object_key();
  req.op = ParseOperationType(request.op_type());
  req.data_path = ParseDataPath(request.data_path());
  req.buffer_type = request.buffer_type();
  req.offset = request.offset();
  req.expected_size = request.expected_size();
  req.idempotency_key = request.idempotency_key();
  req.is_multipart_part = request.is_multipart_part();
  return req;
}

void FillOpenSessionResponse(
    const core::OpenSessionResult& outcome,
    ::us3_turbo_access::gateway::OpenSessionResponse* response) {
  const auto& session = *outcome.session;
  response->set_request_id(session.request_id);
  response->set_session_id(session.session_id);
  response->set_ticket(session.ticket);
  response->set_gateway_id(session.gateway_id);
  response->set_expire_at(session.expire_at);
}

}  // namespace

ControlPlaneService::ControlPlaneService(core::SessionStore& sessions,
                                         core::MetadataService& metadata,
                                         core::SessionOpener& session_opener,
                                         data_path::gds::GdsExecutor* gds_executor,
                                         core::multipart::MultipartAppService& multipart_app,
                                         core::multipart::MultipartCoordinator& multipart_coord,
                                         runtime::IoWorkerPool& io_pool,
                                         std::shared_ptr<spdlog::logger> logger)
    : sessions_(sessions),
      metadata_(metadata),
      session_opener_(session_opener),
      gds_executor_(gds_executor),
      multipart_app_(multipart_app),
      multipart_coord_(multipart_coord),
      io_pool_(io_pool),
      logger_(std::move(logger)) {}

void ControlPlaneService::OpenSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::OpenSessionRequest* request,
    ::us3_turbo_access::gateway::OpenSessionResponse* response,
    google::protobuf::Closure* done) {
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  // 转派到 io_pool 异步执行。
  io_pool_.Submit(std::bind(&ControlPlaneService::HandleOpenSession, this,
                            cntl, request, response, done));
}

// 解码请求，调用 SessionOpener，写响应。
void ControlPlaneService::HandleOpenSession(
    brpc::Controller* cntl,
    const ::us3_turbo_access::gateway::OpenSessionRequest* request,
    ::us3_turbo_access::gateway::OpenSessionResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  common::ScopedLatency latency(common::metrics().open_session_latency_us);
  auto result = session_opener_.Open(ToOpenSessionParams(*request));
  if (!result.success()) {
    common::metrics().open_session_fail_total << 1;
    cntl->SetFailed(result.error().message);
    return;
  }
  common::metrics().open_session_total << 1;
  FillOpenSessionResponse(result.value(), response);
}

void ControlPlaneService::HeadObject(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::HeadObjectRequest* request,
    ::us3_turbo_access::gateway::HeadObjectResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  auto head = metadata_.Head(request->bucket(), request->object_key());
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

void ControlPlaneService::GdsGet(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::GdsChunkRequest* request,
    ::us3_turbo_access::gateway::GdsChunkResponse* response,
    google::protobuf::Closure* done) {
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  // 转派到 io_pool 异步执行。
  io_pool_.Submit(std::bind(&ControlPlaneService::HandleGdsGet, this,
                            cntl, request, response, done));
}

// GDS GET chunk：校验 → 推进 session 状态 → metadata.Head → 触发 RDMA。
void ControlPlaneService::HandleGdsGet(
    brpc::Controller* cntl,
    const ::us3_turbo_access::gateway::GdsChunkRequest* request,
    ::us3_turbo_access::gateway::GdsChunkResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  common::ScopedLatency latency(common::metrics().gds_get_latency_us);

  if (gds_executor_ == nullptr || !gds_executor_->available()) {
    common::metrics().gds_get_fail_total << 1;
    cntl->SetFailed("gds-cuobject service is not available on gateway");
    return;
  }
  auto session = ResolveSession(sessions_, *request);
  if (session == nullptr) {
    common::metrics().gds_get_fail_total << 1;
    cntl->SetFailed("gds-cuobject session not found");
    return;
  }
  if (request->rdma_token().empty()) {
    common::metrics().gds_get_fail_total << 1;
    cntl->SetFailed("missing rdma token for gds-cuobject request");
    return;
  }
  // 第一次到达此 session 的 chunk：CAS kOpened→kActive；
  // 之后的 chunk 是 no-op。BumpActive 不会因状态过期而失败，所以忽略返回值。
  // 首个 chunk 推进 kOpened→kActive；之后是 no-op。
  (void)sessions_.BumpActive(session->session_id);
  auto head = metadata_.Head(request->bucket(), request->object_key());
  if (!head.success()) {
    common::metrics().gds_get_fail_total << 1;
    (void)sessions_.MarkFailed(session->session_id);
    cntl->SetFailed(head.error().message);
    return;
  }
  auto chunk = gds_executor_->GetChunk(*session, request->rdma_token(),
                                       request->chunk_offset(),
                                       request->chunk_size());
  if (!chunk.success()) {
    common::metrics().gds_get_fail_total << 1;
    (void)sessions_.MarkFailed(session->session_id);
    cntl->SetFailed(chunk.error().message);
    return;
  }
  common::metrics().gds_get_total << 1;
  common::metrics().gds_get_bytes << static_cast<std::int64_t>(request->chunk_size());
  FillGdsResponse(session->gateway_id, "completed", chunk.value().rdma_reply,
                  head.value().etag, head.value().version,
                  chunk.value().crc32c, response);
}

void ControlPlaneService::GdsPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::GdsChunkRequest* request,
    ::us3_turbo_access::gateway::GdsChunkResponse* response,
    google::protobuf::Closure* done) {
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  // 转派到 io_pool 异步执行。
  io_pool_.Submit(std::bind(&ControlPlaneService::HandleGdsPut, this,
                            cntl, request, response, done));
}

// GDS PUT chunk：按 upload_id 是否为空分流到 multipart 或单对象路径。
void ControlPlaneService::HandleGdsPut(
    brpc::Controller* cntl,
    const ::us3_turbo_access::gateway::GdsChunkRequest* request,
    ::us3_turbo_access::gateway::GdsChunkResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  common::ScopedLatency latency(common::metrics().gds_put_latency_us);

  if (gds_executor_ == nullptr || !gds_executor_->available()) {
    common::metrics().gds_put_fail_total << 1;
    cntl->SetFailed("gds-cuobject service is not available on gateway");
    return;
  }
  auto session = ResolveSession(sessions_, *request);
  if (session == nullptr) {
    common::metrics().gds_put_fail_total << 1;
    cntl->SetFailed("gds-cuobject session not found");
    return;
  }
  if (request->rdma_token().empty()) {
    common::metrics().gds_put_fail_total << 1;
    cntl->SetFailed("missing rdma token for gds-cuobject request");
    return;
  }
  (void)sessions_.BumpActive(session->session_id);
  // multipart 分支
  if (!request->upload_id().empty()) {
    auto lookup = multipart_coord_.Lookup(request->upload_id());
    if (!lookup.success()) {
      common::metrics().gds_put_fail_total << 1;
      (void)sessions_.MarkFailed(session->session_id);
      cntl->SetFailed(lookup.error().message);
      return;
    }
    auto upload = lookup.value();
    auto part_etag = gds_executor_->PutPart(
        *session, request->rdma_token(), upload->backend_upload_id,
        request->part_number(), request->chunk_offset(),
        request->chunk_size(), request->checksum_policy());
    if (!part_etag.success()) {
      common::metrics().gds_put_fail_total << 1;
      (void)sessions_.MarkFailed(session->session_id);
      cntl->SetFailed(part_etag.error().message);
      return;
    }
    // 登记 part 进度，供 CompleteUpload 校验。
    multipart_coord_.RegisterPart(*upload, request->part_number(),
                            request->chunk_offset(), request->chunk_size(),
                            part_etag.value());
    common::metrics().gds_put_total << 1;
    common::metrics().gds_put_bytes << static_cast<std::int64_t>(request->chunk_size());
    FillGdsResponse(session->gateway_id, "completed", "gds-cuobject-rdma-read",
                    part_etag.value(), /*version=*/"", /*crc32c=*/0, response);
    return;
  }
  // 单对象分支
  auto written = gds_executor_->PutChunk(*session, request->rdma_token(),
                                         request->chunk_offset(),
                                         request->chunk_size());
  if (!written.success()) {
    common::metrics().gds_put_fail_total << 1;
    (void)sessions_.MarkFailed(session->session_id);
    cntl->SetFailed(written.error().message);
    return;
  }
  common::metrics().gds_put_total << 1;
  common::metrics().gds_put_bytes << static_cast<std::int64_t>(request->chunk_size());
  FillGdsResponse(session->gateway_id, "completed", "gds-cuobject-rdma-read",
                  written.value().etag, written.value().version,
                  /*crc32c=*/0, response);
}

// 职责：RPC request/response 适配；不含业务逻辑。
void ControlPlaneService::StartUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::StartUploadRequest* request,
    ::us3_turbo_access::gateway::StartUploadResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  core::multipart::StartUploadParams params;
  params.bucket          = request->bucket();
  params.object_key      = request->object_key();
  if (request->expected_total_size() != 0U) {
    params.expected_total_size =
        static_cast<std::size_t>(request->expected_total_size());
  }
  params.data_path       = request->data_path();
  params.idempotency_key = request->idempotency_key();
  auto out = multipart_app_.StartUpload(params);
  if (!out.success()) {
    cntl->SetFailed(out.error().message);
    return;
  }
  response->set_upload_id(out.value().upload_id);
  response->set_max_part_size(static_cast<std::uint64_t>(out.value().max_part_size));
}

void ControlPlaneService::CompleteUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::CompleteUploadRequest* request,
    ::us3_turbo_access::gateway::CompleteUploadResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  std::vector<backend::PartRecord> parts;
  parts.reserve(static_cast<std::size_t>(request->parts_size()));
  for (const auto& p : request->parts()) {
    backend::PartRecord pr;
    pr.part_number = p.part_number();
    pr.etag = p.etag();
    parts.push_back(std::move(pr));
  }
  auto meta = multipart_app_.CompleteUpload(request->upload_id(), parts,
                                        request->data_path());
  if (!meta.success()) {
    cntl->SetFailed(meta.error().message);
    return;
  }
  response->set_etag(meta.value().etag);
  response->set_version(meta.value().version);
  response->set_content_length(meta.value().content_length);
}

void ControlPlaneService::AbortUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::AbortUploadRequest* request,
    ::us3_turbo_access::gateway::AbortUploadResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  auto result = multipart_app_.AbortUpload(request->upload_id(),
                                       request->data_path());
  if (!result.success()) {
    cntl->SetFailed(result.error().message);
    return;
  }
}

// 通知 server 把 session 标记 failed，触发 sweeper 提前回收（无需等 TTL）。
// 用于 client 重试 OpenSession 前清理上一次失败的 session，避免 server 端
// session 积压。不存在的 session 视为 no-op，返回 erased=false。
void ControlPlaneService::AbortSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::AbortSessionRequest* request,
    ::us3_turbo_access::gateway::AbortSessionResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  (void)cntl_base;  // 不存在的 session 视为 no-op；不让 RPC 失败
  auto marked = sessions_.MarkFailed(request->session_id());
  // success 表示找到并 mark；找不到 session 不报错（best-effort）。
  response->set_erased(marked.success() && marked.value() != nullptr);
}

}  // namespace us3_turbo_access::gateway::api
