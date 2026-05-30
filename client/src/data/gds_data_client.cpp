#include "client/src/data/gds_data_client.h"

#include <utility>

#include <brpc/controller.h>

#include "client/src/core/common/brpc_channel.h"  // ApplyRequestHeaders
#include "client/src/core/common/channel_registry.h"
#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

GdsDataClient::GdsDataClient(ChannelRegistry& registry, const ClientOptions& options)
    : registry_(registry), options_(options) {}

Result<bool> GdsDataClient::Initialize() {
  if (initialized()) {
    return Result<bool>::Success(true);
  }
  auto* ch = registry_.baidu_std();
  if (ch == nullptr) {
    return Result<bool>::Failure(MakeInvalidArgument(
        "GdsDataClient: shared baidu_std channel is not initialized"));
  }
  stub_ = std::make_unique<us3_turbo_access::gateway::ControlPlaneService_Stub>(ch);
  return Result<bool>::Success(true);
}

void GdsDataClient::Shutdown() {
  stub_.reset();
}

bool GdsDataClient::initialized() const { return stub_ != nullptr; }

Result<us3_turbo_access::gateway::GdsChunkResponse> GdsDataClient::GdsChunk(
    const ChunkOp& request) const {
  if (!initialized()) {
    return Result<us3_turbo_access::gateway::GdsChunkResponse>::Failure(
        MakeNotInitialized("GDS data client"));
  }

  brpc::Controller controller;
  ApplyRequestHeaders(controller, request.context);

  us3_turbo_access::gateway::GdsChunkRequest rpc_request;
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
  rpc_request.set_upload_id(request.upload_id);
  rpc_request.set_part_number(request.part_number);
  for (const auto& [key, value] : request.object.extra_headers) {
    (*rpc_request.mutable_extra_headers())[key] = value;
  }

  us3_turbo_access::gateway::GdsChunkResponse rpc_response;
  if (request.operation == OperationType::kGet) {
    stub_->GdsGet(&controller, &rpc_request, &rpc_response, nullptr);
  } else {
    stub_->GdsPut(&controller, &rpc_request, &rpc_response, nullptr);
  }

  auto status = CheckRpcFailure(controller, "Failed to execute GDS chunk RPC",
                                request.object.data_path, request.request_id);
  if (!status.success()) {
    return Result<us3_turbo_access::gateway::GdsChunkResponse>::Failure(status.error());
  }
  return Result<us3_turbo_access::gateway::GdsChunkResponse>::Success(std::move(rpc_response));
}

}  // namespace us3_turbo_access::client
