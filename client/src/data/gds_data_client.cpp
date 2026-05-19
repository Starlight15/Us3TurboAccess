#include "client/src/data/gds_data_client.h"

#include <utility>

#include <brpc/controller.h>

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

GdsDataClient::GdsDataClient(const ClientOptions& options) : channel_(options) {}

Result<bool> GdsDataClient::Initialize() {
  if (initialized()) {
    return Result<bool>::Success(true);
  }
  auto init = channel_.Initialize();
  if (!init.success()) {
    return init;
  }
  stub_ = std::make_unique<fusion_access::gateway::ControlPlaneService_Stub>(channel_.channel());
  return Result<bool>::Success(true);
}

void GdsDataClient::Shutdown() {
  stub_.reset();
  channel_.Shutdown();
}

bool GdsDataClient::initialized() const { return channel_.ready() && stub_ != nullptr; }

Result<fusion_access::gateway::ExecuteGdsChunkResponse> GdsDataClient::ExecuteGdsChunk(
    const ChunkTransferRequest& request) const {
  if (!initialized()) {
    return Result<fusion_access::gateway::ExecuteGdsChunkResponse>::Failure(
        MakeNotInitialized("GDS data client"));
  }

  brpc::Controller controller;
  ApplyRequestHeaders(controller, request.context);

  fusion_access::gateway::ExecuteGdsChunkRequest rpc_request;
  rpc_request.set_request_id(request.request_id);
  rpc_request.set_session_id(request.session_id);
  rpc_request.set_transfer_ticket(request.transfer_ticket);
  rpc_request.set_bucket(request.object.object.bucket);
  rpc_request.set_object_key(request.object.object.key);
  rpc_request.set_data_path(std::string(ToString(request.object.data_path)));
  rpc_request.set_buffer_type(std::string(ToString(request.object.buffer_type)));
  rpc_request.set_checksum_policy(request.object.checksum_policy);
  rpc_request.set_chunk_offset(request.chunk_offset);
  rpc_request.set_chunk_size(request.chunk_size);
  rpc_request.set_rdma_token(request.rdma_token);
  for (const auto& [key, value] : request.object.extra_headers) {
    (*rpc_request.mutable_extra_headers())[key] = value;
  }

  fusion_access::gateway::ExecuteGdsChunkResponse rpc_response;
  if (request.operation == OperationType::kGet) {
    stub_->ExecuteGdsGet(&controller, &rpc_request, &rpc_response, nullptr);
  } else {
    stub_->ExecuteGdsPut(&controller, &rpc_request, &rpc_response, nullptr);
  }

  auto status = CheckRpcFailure(controller, "Failed to execute GDS chunk RPC",
                                request.object.data_path, request.request_id);
  if (!status.success()) {
    return Result<fusion_access::gateway::ExecuteGdsChunkResponse>::Failure(status.error());
  }
  return Result<fusion_access::gateway::ExecuteGdsChunkResponse>::Success(std::move(rpc_response));
}

}  // namespace us3_turbo_access::client
