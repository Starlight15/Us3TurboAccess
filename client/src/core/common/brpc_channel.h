#pragma once

#include <memory>
#include <string>

#include <brpc/channel.h>
#include <brpc/controller.h>

#include "client/src/core/contracts/rpc_context.h"
#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

[[nodiscard]] std::string TrimTrailingSlash(std::string endpoint);
void ApplyRequestHeaders(brpc::Controller& controller, const RpcCallMetadata& context);
[[nodiscard]] Result<bool> CheckRpcFailure(const brpc::Controller& controller,
                                           const std::string& message,
                                           DataPath path,
                                           const std::string& request_id);

/**
 * @brief 包装 brpc::Channel 的生命周期与 ClientOptions 配置。
 *
 * 让具体的 RPC client 通过聚合该类持有 channel，避免共用基类做私有继承。
 */
class BrpcChannel {
 public:
  explicit BrpcChannel(const ClientOptions& options);

  [[nodiscard]] Result<bool> Initialize();
  void Shutdown();
  [[nodiscard]] bool ready() const;
  [[nodiscard]] brpc::Channel* channel() const { return channel_.get(); }
  [[nodiscard]] const ClientOptions& options() const { return options_; }

 private:
  const ClientOptions& options_;
  std::string endpoint_;
  std::unique_ptr<brpc::Channel> channel_;
};

}  // namespace us3_turbo_access::client
