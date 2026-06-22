#include "client/src/core/common/brpc_channel.h"

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

std::string TrimTrailingSlash(std::string endpoint) {
  while (!endpoint.empty() && endpoint.back() == '/') {
    endpoint.pop_back();
  }
  return endpoint;
}

void ApplyRequestHeaders(brpc::Controller& controller, const RpcCallMetadata& context) {
  for (const auto& [key, value] : context.default_headers) {
    controller.http_request().SetHeader(key, value);
  }
  if (!context.client_id.empty()) {
    controller.http_request().SetHeader("x-fa-client-id", context.client_id);
  }
  if (!context.bearer_token.empty()) {
    controller.http_request().SetHeader("Authorization", "Bearer " + context.bearer_token);
  }
  controller.set_timeout_ms(static_cast<int>(context.timeout.count()));
}

void ApplyRequestTimeout(brpc::Controller& controller,
                         const ClientOptions& options) {
  // 端到端 request_timeout 覆盖 channel 默认（baidu_std channel 初始化时
  // 取的是 default_timeout，30s）；这里强制使用 request_timeout（默认 5min）。
  controller.set_timeout_ms(
      static_cast<int>(options.request_timeout.count()));
}

Result<bool> CheckRpcFailure(const brpc::Controller& controller,
                             const std::string& message,
                             DataFlow path,
                             const std::string& request_id) {
  if (!controller.Failed()) {
    return Result<bool>::Success(true);
  }
  return Result<bool>::Failure(MakeRpcFailure(controller, message, path, request_id));
}

BrpcChannel::BrpcChannel(const ClientOptions& options) : options_(options) {}

Result<bool> BrpcChannel::Initialize() {
  if (ready()) {
    return Result<bool>::Success(true);
  }
  if (options_.endpoint.empty()) {
    return Result<bool>::Failure(MakeInvalidArgument("endpoint must not be empty"));
  }

  endpoint_ = TrimTrailingSlash(options_.endpoint);
  auto channel = std::make_unique<brpc::Channel>();
  brpc::ChannelOptions channel_options;
  channel_options.protocol = "baidu_std";
  channel_options.connect_timeout_ms = static_cast<int>(options_.default_timeout.count());
  channel_options.timeout_ms = static_cast<int>(options_.default_timeout.count());
  channel_options.max_retry = 2;
  if (channel->Init(endpoint_.c_str(), nullptr, &channel_options) != 0) {
    return Result<bool>::Failure(
        MakeError(ErrorCode::kRpcError, "Failed to initialize brpc channel: " + endpoint_, true));
  }
  channel_ = std::move(channel);
  return Result<bool>::Success(true);
}

void BrpcChannel::Shutdown() {
  channel_.reset();
  endpoint_.clear();
}

bool BrpcChannel::ready() const { return channel_ != nullptr; }

}  // namespace us3_turbo_access::client
