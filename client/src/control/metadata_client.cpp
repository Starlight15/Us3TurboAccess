#include "client/src/control/metadata_client.h"

#include <utility>

#include <brpc/controller.h>

#include "client/src/core/common/channel_registry.h"
#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

MetadataClient::MetadataClient(ChannelRegistry& registry,
                                const ClientOptions& options)
    : registry_(registry), options_(options) {}

Result<bool> MetadataClient::Initialize() {
  if (initialized()) {
    return Result<bool>::Success(true);
  }
  auto* ch = registry_.baidu_std();
  if (ch == nullptr) {
    return Result<bool>::Failure(MakeInvalidArgument(
        "MetadataClient: shared baidu_std channel is not initialized"));
  }
  stub_ = std::make_unique<us3_turbo_access::gateway::ControlPlaneService_Stub>(ch);
  return Result<bool>::Success(true);
}

void MetadataClient::Shutdown() {
  // 共享 Channel：本类只析构 stub，channel 由 ChannelRegistry 管。
  stub_.reset();
}

bool MetadataClient::initialized() const { return stub_ != nullptr; }

Result<us3_turbo_access::gateway::OpenSessionResponse>
MetadataClient::OpenTransferSession(const SessionOpening& request) const {
  if (!initialized()) {
    return Result<us3_turbo_access::gateway::OpenSessionResponse>::Failure(
        MakeNotInitialized("Metadata client"));
  }

  brpc::Controller controller;
  ApplyRequestHeaders(controller, request.context);

  us3_turbo_access::gateway::OpenSessionRequest rpc_request;
  rpc_request.set_request_id(request.request_id);
  rpc_request.set_session_id(request.session_id);
  rpc_request.set_bucket(request.object.bucket);
  rpc_request.set_object_key(request.object.key);
  rpc_request.set_op_type(std::string(ToString(request.operation)));
  rpc_request.set_data_path(std::string(ToString(request.data_path)));
  rpc_request.set_buffer_type(std::string(ToString(request.buffer_type)));
  rpc_request.set_offset(request.offset);
  rpc_request.set_expected_size(request.length.value_or(0));
  rpc_request.set_idempotency_key(request.idempotency_key);
  rpc_request.set_is_multipart_part(request.is_multipart_part);

  us3_turbo_access::gateway::OpenSessionResponse rpc_response;
  stub_->OpenSession(&controller, &rpc_request, &rpc_response, nullptr);

  auto status = CheckRpcFailure(controller, "Failed to open transfer session", request.data_path,
                                request.request_id);
  if (!status.success()) {
    return Result<us3_turbo_access::gateway::OpenSessionResponse>::Failure(status.error());
  }
  return Result<us3_turbo_access::gateway::OpenSessionResponse>::Success(
      std::move(rpc_response));
}

Result<ObjectMetadata> MetadataClient::HeadObject(const ObjectId& object) const {
  if (!initialized()) {
    return Result<ObjectMetadata>::Failure(MakeNotInitialized("Metadata client"));
  }

  const ClientOptions& options = options_;
  const RpcCallMetadata context{.client_id = options.client_id,
                                  .bearer_token = options.bearer_token,
                                  .default_headers = options.default_headers,
                                  .timeout = options.default_timeout};

  brpc::Controller controller;
  ApplyRequestHeaders(controller, context);

  us3_turbo_access::gateway::HeadObjectRequest rpc_request;
  rpc_request.set_bucket(object.bucket);
  rpc_request.set_object_key(object.key);

  us3_turbo_access::gateway::HeadObjectResponse rpc_response;
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

// RPC 参数构造 helper：ObjectDescriptor → protobuf request。
static us3_turbo_access::gateway::StartUploadRequest BuildStartUploadRequest(
    const ObjectDescriptor& desc) {
  us3_turbo_access::gateway::StartUploadRequest req;
  req.set_bucket(desc.object.bucket);
  req.set_object_key(desc.object.key);
  req.set_expected_total_size(
      static_cast<std::uint64_t>(desc.expected_total_size.value_or(0)));
  req.set_data_path(std::string(ToString(desc.data_path)));
  req.set_idempotency_key(desc.idempotency_key);
  return req;
}

// 职责：RPC 封装与响应解析；不含业务逻辑。
Result<StartUploadResult> MetadataClient::RpcStartUpload(
    const ObjectDescriptor& desc) const {
  if (!initialized()) {
    return Result<StartUploadResult>::Failure(MakeNotInitialized("Metadata client"));
  }
  const RpcCallMetadata context{.client_id = options_.client_id,
                                  .bearer_token = options_.bearer_token,
                                  .default_headers = options_.default_headers,
                                  .timeout = options_.default_timeout};
  brpc::Controller controller;
  ApplyRequestHeaders(controller, context);

  auto req = BuildStartUploadRequest(desc);
  us3_turbo_access::gateway::StartUploadResponse resp;
  stub_->StartUpload(&controller, &req, &resp, nullptr);
  auto status = CheckRpcFailure(controller, "RpcStartUpload failed", desc.data_path, "");
  if (!status.success()) {
    return Result<StartUploadResult>::Failure(status.error());
  }
  StartUploadResult out;
  out.upload_id     = resp.upload_id();
  out.max_part_size = static_cast<std::size_t>(resp.max_part_size());
  return Result<StartUploadResult>::Success(std::move(out));
}

Result<CompleteUploadOutcome> MetadataClient::CompleteUpload(
    const std::string& upload_id,
    const std::vector<PartCompletion>& parts,
    DataPath data_path) const {
  if (!initialized()) {
    return Result<CompleteUploadOutcome>::Failure(MakeNotInitialized("Metadata client"));
  }
  const ClientOptions& options = options_;
  const RpcCallMetadata context{.client_id = options.client_id,
                                  .bearer_token = options.bearer_token,
                                  .default_headers = options.default_headers,
                                  .timeout = options.default_timeout};
  brpc::Controller controller;
  ApplyRequestHeaders(controller, context);

  us3_turbo_access::gateway::CompleteUploadRequest req;
  req.set_upload_id(upload_id);
  req.set_data_path(std::string(ToString(data_path)));
  for (const auto& p : parts) {
    auto* pe = req.add_parts();
    pe->set_part_number(p.part_number);
    pe->set_etag(p.etag);
  }
  us3_turbo_access::gateway::CompleteUploadResponse resp;
  stub_->CompleteUpload(&controller, &req, &resp, nullptr);
  auto status = CheckRpcFailure(controller, "CompleteUpload RPC failed",
                                data_path, "");
  if (!status.success()) {
    return Result<CompleteUploadOutcome>::Failure(status.error());
  }
  CompleteUploadOutcome out;
  out.etag = resp.etag();
  out.version = resp.version();
  out.content_length = static_cast<std::size_t>(resp.content_length());
  return Result<CompleteUploadOutcome>::Success(std::move(out));
}

Result<bool> MetadataClient::AbortUpload(const std::string& upload_id,
                                          DataPath data_path) const {
  if (!initialized()) {
    return Result<bool>::Failure(MakeNotInitialized("Metadata client"));
  }
  const ClientOptions& options = options_;
  const RpcCallMetadata context{.client_id = options.client_id,
                                  .bearer_token = options.bearer_token,
                                  .default_headers = options.default_headers,
                                  .timeout = options.default_timeout};
  brpc::Controller controller;
  ApplyRequestHeaders(controller, context);

  us3_turbo_access::gateway::AbortUploadRequest req;
  req.set_upload_id(upload_id);
  req.set_data_path(std::string(ToString(data_path)));
  us3_turbo_access::gateway::AbortUploadResponse resp;
  stub_->AbortUpload(&controller, &req, &resp, nullptr);
  auto status = CheckRpcFailure(controller, "AbortUpload RPC failed",
                                data_path, "");
  if (!status.success()) {
    return Result<bool>::Failure(status.error());
  }
  return Result<bool>::Success(true);
}

Result<bool> MetadataClient::AbortSession(const std::string& session_id) const {
  if (!initialized()) {
    return Result<bool>::Failure(MakeNotInitialized("Metadata client"));
  }
  const ClientOptions& options = options_;
  const RpcCallMetadata context{.client_id = options.client_id,
                                  .bearer_token = options.bearer_token,
                                  .default_headers = options.default_headers,
                                  .timeout = options.default_timeout};
  brpc::Controller controller;
  ApplyRequestHeaders(controller, context);

  us3_turbo_access::gateway::AbortSessionRequest req;
  req.set_session_id(session_id);
  us3_turbo_access::gateway::AbortSessionResponse resp;
  stub_->AbortSession(&controller, &req, &resp, nullptr);
  // best-effort：RPC 失败也算 success(false)，不让重试失败干扰主流程
  if (controller.Failed()) {
    return Result<bool>::Success(false);
  }
  return Result<bool>::Success(resp.erased());
}

}  // namespace us3_turbo_access::client
