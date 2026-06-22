#include "api/rdma_data_plane_service.h"

#include <utility>

#include <brpc/closure_guard.h>
#include <spdlog/logger.h>

#include "data_path/ucx/ucx_executor.h"
#include "data_path/ucx/ucx_multipart_path_handler.h"

namespace us3_turbo_access::gateway::api {

RdmaDataPlaneService::RdmaDataPlaneService(
    data_flow::ucx::UcxExecutor& executor,
    data_flow::ucx::UcxMultipartPathHandler& multipart_handler,
    std::shared_ptr<spdlog::logger> logger)
    : executor_(executor),
      multipart_handler_(multipart_handler),
      logger_(std::move(logger)) {}

void RdmaDataPlaneService::DiscoverRdmaEndpoint(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::RdmaDiscoverRequest* request,
    ::us3_turbo_access::gateway::RdmaDiscoverResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  // RMA WRITE 模式：client 上报 transfer_bytes，gateway 分配 buffer 并下发 gw_raddr/rkey
  auto info = executor_.PrepareTransfer(
      request->session_id(),
      request->transfer_bytes());
  if (!info.success()) { cntl->SetFailed(info.error().message); return; }

  response->set_host(info.value().host);
  response->set_port(info.value().ucx_port);
  response->set_max_msg_bytes(info.value().max_msg_bytes);
  response->set_ucx_port(info.value().ucx_port);
  response->set_gw_raddr(info.value().gw_raddr);
  response->set_gw_packed_rkey(info.value().gw_packed_rkey.data(),
                                info.value().gw_packed_rkey.size());
}

void RdmaDataPlaneService::BindSessionToConnection(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::RdmaBindRequest*,
    ::us3_turbo_access::gateway::RdmaBindResponse*,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  static_cast<brpc::Controller*>(cntl_base)->SetFailed(
      "BindSessionToConnection not used in UCX RMA path");
}

void RdmaDataPlaneService::CommitObject(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::RdmaCommitRequest* request,
    ::us3_turbo_access::gateway::RdmaCommitResponse* response,
    google::protobuf::Closure* done) {
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  bool sync = executor_.CommitObjectAsync(
      request->session_id(),
      request->bytes_transferred(),
      request->client_checksum(),
      [cntl, response, done](Result<data_flow::ucx::UcxCommitInfo> info) {
        if (!info.success()) { cntl->SetFailed(info.error().message); }
        else {
          response->set_etag(info.value().etag);
          response->set_version(info.value().version);
        }
        done->Run();
      });

  if (!sync) return;
}

void RdmaDataPlaneService::CommitPart(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::RdmaCommitPartRequest* request,
    ::us3_turbo_access::gateway::RdmaCommitPartResponse* response,
    google::protobuf::Closure* done) {
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  // Multipart part commit：业务编排委托给 UcxMultipartPathHandler
  bool sync = multipart_handler_.CommitPartAsync(
      request->session_id(), request->upload_id(),
      request->part_number(), request->bytes_transferred(),
      request->client_checksum(),
      [cntl, response, done](Result<data_flow::ucx::UcxMultipartPartResult> info) {
        if (!info.success()) { cntl->SetFailed(info.error().message); }
        else { response->set_part_etag(info.value().part_etag); }
        done->Run();
      });

  if (!sync) return;
}

void RdmaDataPlaneService::AbortSession(
    google::protobuf::RpcController* cntl_base,
    const ::us3_turbo_access::gateway::RdmaAbortRequest* request,
    ::us3_turbo_access::gateway::RdmaAbortResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  auto info = executor_.AbortSession(request->session_id());
  if (!info.success()) { cntl->SetFailed(info.error().message); return; }
  response->set_erased(info.value());
}

}  // namespace us3_turbo_access::gateway::api
