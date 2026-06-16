#include "api/control_plane_service.h"

#include <functional>
#include <utility>

#include <brpc/closure_guard.h>
#include <spdlog/logger.h>

#include "api/conversions.h"
#include "common/metrics.h"
#include "backend/backend.h"
#include "core/metadata/metadata_service.h"
#include "core/multipart/multipart_app_service.h"
#include "core/session/session_app_service.h"
#include "core/session_opener/session_opener.h"
#include "data_path/gds/gds_executor.h"
#include "data_path/gds/gds_multipart_path_handler.h"
#include "runtime/io_worker_pool.h"
#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway::api {

namespace {

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

// ---------------------------------------------------------------------------

Result<std::shared_ptr<core::Session>>
ControlPlaneService::PrepareGdsChunk(
    brpc::Controller* cntl,
    const ::us3_turbo_access::gateway::GdsChunkRequest* request,
    const char* operation_name) {
  auto& fail_metric = (std::string_view(operation_name) == "gds_get")
      ? common::metrics().gds_get_fail_total
      : common::metrics().gds_put_fail_total;

  if (gds_executor_ == nullptr || !gds_executor_->available()) {
    fail_metric << 1;
    cntl->SetFailed("gds-cuobject service is not available on gateway");
    return Result<std::shared_ptr<core::Session>>::Failure(
        MakeError(ErrorCode::kInternal, "gds-cuobject service is not available on gateway"));
  }
  auto session = session_app_.ResolveForGdsChunk(
      request->session_id(), request->transfer_ticket());
  if (session == nullptr) {
    fail_metric << 1;
    cntl->SetFailed("gds-cuobject session not found");
    return Result<std::shared_ptr<core::Session>>::Failure(
        MakeError(ErrorCode::kNotFound, "gds-cuobject session not found"));
  }
  if (request->rdma_token().empty()) {
    fail_metric << 1;
    cntl->SetFailed("missing rdma token for gds-cuobject request");
    return Result<std::shared_ptr<core::Session>>::Failure(
        MakeError(ErrorCode::kInvalidArgument, "missing rdma token for gds-cuobject request"));
  }
  session_app_.BumpActive(session->session_id);
  return Result<std::shared_ptr<core::Session>>::Success(std::move(session));
}

// ---------------------------------------------------------------------------

ControlPlaneService::ControlPlaneService(core::SessionAppService& session_app,
                                         core::MetadataService& metadata,
                                         core::SessionOpener& session_opener,
                                         data_path::gds::GdsExecutor* gds_executor,
                                         data_path::gds::GdsMultipartPathHandler* gds_multipart_handler,
                                         core::multipart::MultipartAppService& multipart_app,
                                         runtime::IoWorkerPool& io_pool,
                                         std::shared_ptr<spdlog::logger> logger)
    : session_app_(session_app),
      metadata_(metadata),
      session_opener_(session_opener),
      gds_executor_(gds_executor),
      gds_multipart_handler_(gds_multipart_handler),
      multipart_app_(multipart_app),
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

  auto session_result = PrepareGdsChunk(cntl, request, "gds_get");
  if (!session_result.success()) return;
  auto session = session_result.value();

  auto head = metadata_.Head(request->bucket(), request->object_key());
  if (!head.success()) {
    common::metrics().gds_get_fail_total << 1;
    session_app_.MarkFailed(*session);
    cntl->SetFailed(head.error().message);
    return;
  }
  auto chunk = gds_executor_->GetChunk(*session, request->rdma_token(),
                                       request->chunk_offset(),
                                       request->chunk_size());
  if (!chunk.success()) {
    common::metrics().gds_get_fail_total << 1;
    session_app_.MarkFailed(*session);
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

  auto session_result = PrepareGdsChunk(cntl, request, "gds_put");
  if (!session_result.success()) return;
  auto session = session_result.value();

  // multipart 分支：编排委托给 GdsMultipartPathHandler
  if (!request->upload_id().empty()) {
    if (gds_multipart_handler_ == nullptr) {
      common::metrics().gds_put_fail_total << 1;
      cntl->SetFailed("gds multipart handler not available");
      return;
    }
    auto result = gds_multipart_handler_->UploadPart(
        *session, request->rdma_token(), request->upload_id(),
        request->part_number(), request->chunk_offset(),
        request->chunk_size(), request->checksum_policy());
    if (!result.success()) {
      common::metrics().gds_put_fail_total << 1;
      session_app_.MarkFailed(*session);
      cntl->SetFailed(result.error().message);
      return;
    }
    common::metrics().gds_put_total << 1;
    common::metrics().gds_put_bytes << static_cast<std::int64_t>(request->chunk_size());
    FillGdsResponse(session->gateway_id, "completed",
                    result.value().rdma_reply, result.value().part_etag,
                    /*version=*/"", result.value().crc32c, response);
    return;
  }
  // 单对象分支
  auto written = gds_executor_->PutChunk(*session, request->rdma_token(),
                                         request->chunk_offset(),
                                         request->chunk_size());
  if (!written.success()) {
    common::metrics().gds_put_fail_total << 1;
    session_app_.MarkFailed(*session);
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
  auto params = ToStartUploadParams(*request);
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
  auto parts = ToPartRecords(request->parts());
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

void ControlPlaneService::AbortSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::AbortSessionRequest* request,
    ::us3_turbo_access::gateway::AbortSessionResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  (void)cntl_base;  // 不存在的 session 视为 no-op；不让 RPC 失败
  auto erased = session_app_.MarkFailedById(request->session_id());
  response->set_erased(erased);
}

}  // namespace us3_turbo_access::gateway::api
