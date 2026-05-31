#pragma once

#include <memory>
#include <optional>
#include <string>

#include <brpc/channel.h>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

/**
 * 三个 baidu_std client 共用同一个 brpc::Channel：
 *   MetadataClient        (ControlPlaneService_Stub)
 *   GdsDataClient         (ControlPlaneService_Stub)
 *   RdmaDataPlaneClient   (RdmaDataPlaneService_Stub)
 *
 * brpc::Channel 是线程安全的，可被多 Stub 共享；同 endpoint 上的多
 * stub 复用底层连接池，减少 TCP 连接数（3 → 1）和资源开销。
 *
 * HTTP channel 单独管理：protocol="http" 与 baidu_std 不兼容，brpc
 * 不允许一个 Channel 跑两种协议。
 *
 * 生命周期：Initialize 一次成功后多 RPC 复用；Shutdown 释放底层 channel。
 */
class ChannelRegistry {
 public:
  explicit ChannelRegistry(const ClientOptions& options);

  [[nodiscard]] Result<bool> Initialize();
  void Shutdown();
  [[nodiscard]] bool ready() const;

  /** baidu_std channel，供 MetadataClient / GdsDataClient / RdmaDataPlaneClient 共用。 */
  [[nodiscard]] brpc::Channel* baidu_std() const noexcept { return baidu_.get(); }

  [[nodiscard]] const ClientOptions& options() const noexcept { return options_; }

  /**
   * 构造期 Initialize 失败时保存的错误（之前是 (void) 静默丢弃）；
   * 后续 client.Initialize 调用方可以查这个拿到原始原因，便于诊断
   * "为什么 baidu_std() 返回 nullptr"。无失败时返 std::nullopt。
   */
  [[nodiscard]] std::optional<Error> last_init_error() const { return last_init_error_; }

 private:
  const ClientOptions&            options_;
  std::string                     endpoint_;
  std::unique_ptr<brpc::Channel>  baidu_;
  std::optional<Error>            last_init_error_;
};

}  // namespace us3_turbo_access::client
