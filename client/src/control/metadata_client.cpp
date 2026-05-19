#include "client/src/control/metadata_client.h"

#include <utility>

#include <brpc/controller.h>

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

MetadataClient::MetadataClient(const ClientOptions& options) : channel_(options) {}

Result<bool> MetadataClient::Initialize() {
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

void MetadataClient::Shutdown() {
  stub_.reset();
  channel_.Shutdown();
}

bool MetadataClient::initialized() const { return channel_.ready() && stub_ != nullptr; }

Result<fusion_access::gateway::NegotiateTransferSessionResponse>
MetadataClient::OpenTransferSession(const OpenSessionRequest& request) const {
  if (!initialized()) {
    return Result<fusion_access::gateway::NegotiateTransferSessionResponse>::Failure(
        MakeNotInitialized("Metadata client"));
  }

  brpc::Controller controller;
  ApplyRequestHeaders(controller, request.context);

  fusion_access::gateway::NegotiateTransferSessionRequest rpc_request;
  rpc_request.set_request_id(request.request_id);
  rpc_request.set_session_id(request.session_id);
  rpc_request.set_bucket(request.object.bucket);
  rpc_request.set_object_key(request.object.key);
  rpc_request.set_op_type(std::string(ToString(request.operation)));
  rpc_request.set_data_path(std::string(ToString(request.data_path)));
  rpc_request.set_buffer_type(std::string(ToString(request.buffer_type)));
  rpc_request.set_channel_id(request.channel_id);
  rpc_request.set_offset(request.offset);
  rpc_request.set_expected_size(request.length.value_or(0));
  rpc_request.set_buffer_descriptor(request.buffer_descriptor);
  rpc_request.set_idempotency_key(request.idempotency_key);

  fusion_access::gateway::NegotiateTransferSessionResponse rpc_response;
  stub_->NegotiateTransferSession(&controller, &rpc_request, &rpc_response, nullptr);

  auto status = CheckRpcFailure(controller, "Failed to open transfer session", request.data_path,
                                request.request_id);
  if (!status.success()) {
    return Result<fusion_access::gateway::NegotiateTransferSessionResponse>::Failure(status.error());
  }
  return Result<fusion_access::gateway::NegotiateTransferSessionResponse>::Success(
      std::move(rpc_response));
}

Result<ObjectMetadata> MetadataClient::HeadObject(const ObjectId& object) const {
  if (!initialized()) {
    return Result<ObjectMetadata>::Failure(MakeNotInitialized("Metadata client"));
  }

  const ClientOptions& options = channel_.options();
  const RpcRequestContext context{.client_id = options.client_id,
                                  .bearer_token = options.bearer_token,
                                  .default_headers = options.default_headers,
                                  .timeout = options.default_timeout};

  brpc::Controller controller;
  ApplyRequestHeaders(controller, context);

  fusion_access::gateway::HeadObjectRequest rpc_request;
  rpc_request.set_bucket(object.bucket);
  rpc_request.set_object_key(object.key);

  fusion_access::gateway::HeadObjectResponse rpc_response;
  stub_->HeadObject(&controller, &rpc_request, &rpc_response, nullptr);

  auto status = CheckRpcFailure(controller, "HeadObject RPC failed", DataPath::kGdsCuObject, "");
  if (!status.success()) {
    return Result<ObjectMetadata>::Failure(status.error());
  }

  ObjectMetadata metadata;
  metadata.content_length = static_cast<std::size_t>(rpc_response.content_length());
  metadata.etag = rpc_response.etag();
  metadata.version = rpc_response.version();
  for (const auto& [key, value] : rpc_response.headers()) {
    metadata.headers[key] = value;
  }
  return Result<ObjectMetadata>::Success(std::move(metadata));
}

}  // namespace us3_turbo_access::client
