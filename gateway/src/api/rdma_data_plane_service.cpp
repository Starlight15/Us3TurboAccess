#include "api/rdma_data_plane_service.h"

#include <utility>

#include <brpc/closure_guard.h>
#include <spdlog/logger.h>

#include "data_path/rdma/rdma_executor.h"

namespace us3_turbo_access::gateway::api {

RdmaDataPlaneService::RdmaDataPlaneService(
    data_path::rdma::RdmaExecutor& executor,
    std::shared_ptr<spdlog::logger> logger)
    : executor_(executor), logger_(std::move(logger)) {}

void RdmaDataPlaneService::DiscoverRdmaEndpoint(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::RdmaDiscoverRequest* request,
    ::us3_turbo_access::gateway::RdmaDiscoverResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  auto info = executor_.DiscoverEndpoint(request->session_id());
  if (!info.success()) {
    cntl->SetFailed(info.error().message);
    return;
  }
  response->set_host(info.value().host);
  response->set_port(info.value().port);
  response->set_max_msg_bytes(info.value().max_msg_bytes);
}

void RdmaDataPlaneService::BindSessionToConnection(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::RdmaBindRequest* request,
    ::us3_turbo_access::gateway::RdmaBindResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  auto info = executor_.BindSessionToConnection(request->session_id(),
                                                  request->conn_token());
  if (!info.success()) {
    cntl->SetFailed(info.error().message);
    return;
  }
  response->set_raddr(info.value().raddr);
  response->set_rkey(info.value().rkey);
}

void RdmaDataPlaneService::CommitObject(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::RdmaCommitRequest* request,
    ::us3_turbo_access::gateway::RdmaCommitResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  auto info = executor_.CommitObject(request->session_id(),
                                       request->bytes_transferred(),
                                       request->client_checksum());
  if (!info.success()) {
    cntl->SetFailed(info.error().message);
    return;
  }
  response->set_etag(info.value().etag);
  response->set_version(info.value().version);
}

void RdmaDataPlaneService::AbortSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::RdmaAbortRequest* request,
    ::us3_turbo_access::gateway::RdmaAbortResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  auto info = executor_.AbortSession(request->session_id());
  if (!info.success()) {
    cntl->SetFailed(info.error().message);
    return;
  }
  response->set_erased(info.value());
}

void RdmaDataPlaneService::CommitPart(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::RdmaCommitPartRequest* request,
    ::us3_turbo_access::gateway::RdmaCommitPartResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  auto info = executor_.CommitPart(
      request->session_id(), request->upload_id(), request->part_number(),
      request->bytes_transferred(), request->client_checksum());
  if (!info.success()) {
    cntl->SetFailed(info.error().message);
    return;
  }
  response->set_part_etag(info.value().part_etag);
}

}  // namespace us3_turbo_access::gateway::api
