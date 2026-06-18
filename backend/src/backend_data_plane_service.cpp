#include "backend/src/backend_data_plane_service.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <spdlog/spdlog.h>

namespace us3_turbo_access::backend {

namespace {

constexpr std::string_view kNotImplemented =
    "backend v1: only single-object GdsPut";

[[nodiscard]] std::string Crc32cHex(std::uint32_t crc) {
  char buf[9] = {};
  std::snprintf(buf, sizeof(buf), "%08x", crc);
  return std::string(buf);
}

[[nodiscard]] std::string BuildObjectId(const std::string& bucket,
                                        const std::string& object_key) {
  return bucket + "/" + object_key;
}

}  // namespace

BackendDataPlaneService::BackendDataPlaneService(BackendGdsSink& sink,
                                                 std::string gateway_id)
    : sink_(sink), gateway_id_(std::move(gateway_id)) {}

void BackendDataPlaneService::GdsPut(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::GdsChunkRequest* request,
    ::us3_turbo_access::gateway::GdsChunkResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  // v1 只支持单对象分支：upload_id 必须为空、part_number 为 0。
  if (!request->upload_id().empty()) {
    cntl->SetFailed(std::string(kNotImplemented));
    return;
  }

  // v1 不校验 session/ticket（proxy 是另一进程）；只要求 token 非空、长度合法。
  if (request->rdma_token().empty()) {
    cntl->SetFailed("backend v1: missing rdma_token for GdsPut");
    return;
  }
  // chunk_size 取自 request；校验在 sink 内部（≤ 1 GiB）。
  const auto chunk_size = request->chunk_size();
  if (!sink_.available()) {
    cntl->SetFailed("backend v1: cuObjServer not available");
    return;
  }

  const std::string object_id =
      BuildObjectId(request->bucket(), request->object_key());
  auto outcome =
      sink_.ReceiveAndDiscard(object_id, request->rdma_token(), chunk_size);
  if (!outcome.ok) {
    cntl->SetFailed("backend v1: " + outcome.error);
    return;
  }

  // 合成响应：etag 用 crc32c hex（长度 0 时给 "discard-0"）；version="1"。
  const std::string etag = (outcome.bytes_transferred == 0U)
                               ? std::string("discard-0")
                               : Crc32cHex(outcome.crc32c);
  response->set_selected_gateway(gateway_id_);
  response->set_gateway_id(gateway_id_);
  response->set_transfer_status("completed");
  response->set_rdma_reply("gds-cuobject-rdma-read");
  response->set_etag(etag);
  response->set_version("1");
  response->set_crc32c(outcome.crc32c);

  spdlog::info("backend.gdsput object={} bytes={} crc32c={:x}", object_id,
               outcome.bytes_transferred, outcome.crc32c);
}

// ---------------------------------------------------------------------------
// v1 占位方法：一律返回 "backend v1: only single-object GdsPut"
// ---------------------------------------------------------------------------

void BackendDataPlaneService::OpenSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::OpenSessionRequest* /*request*/,
    ::us3_turbo_access::gateway::OpenSessionResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(std::string(kNotImplemented));
}

void BackendDataPlaneService::HeadObject(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::HeadObjectRequest* /*request*/,
    ::us3_turbo_access::gateway::HeadObjectResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(std::string(kNotImplemented));
}

void BackendDataPlaneService::GdsGet(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::GdsChunkRequest* /*request*/,
    ::us3_turbo_access::gateway::GdsChunkResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(std::string(kNotImplemented));
}

void BackendDataPlaneService::AbortSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::AbortSessionRequest* /*request*/,
    ::us3_turbo_access::gateway::AbortSessionResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(std::string(kNotImplemented));
}

void BackendDataPlaneService::StartUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::StartUploadRequest* /*request*/,
    ::us3_turbo_access::gateway::StartUploadResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(std::string(kNotImplemented));
}

void BackendDataPlaneService::CompleteUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::CompleteUploadRequest* /*request*/,
    ::us3_turbo_access::gateway::CompleteUploadResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(std::string(kNotImplemented));
}

void BackendDataPlaneService::AbortUpload(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::AbortUploadRequest* /*request*/,
    ::us3_turbo_access::gateway::AbortUploadResponse* /*response*/,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  cntl->SetFailed(std::string(kNotImplemented));
}

}  // namespace us3_turbo_access::backend
