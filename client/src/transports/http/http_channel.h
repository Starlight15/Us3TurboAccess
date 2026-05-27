#pragma once

#include <memory>
#include <string>

#include <brpc/channel.h>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

/**
 * brpc HTTP/1.1 客户端 channel 的轻量包装。与 BrpcChannel（baidu_std）平行存在，
 * 二者不共享 brpc::Channel —— HTTP 必须以 protocol="http" 初始化，与 baidu_std
 * 二进制协议不兼容；将来 HTTP 层加 H2 / TLS 也不会牵动控制面通道。
 *
 * 生命周期与 BrpcChannel 同形：Initialize 一次成功后多 RPC 复用，Shutdown 关闭。
 */
class HttpChannel {
 public:
  explicit HttpChannel(const ClientOptions& options);

  [[nodiscard]] Result<bool> Initialize();
  void Shutdown();
  [[nodiscard]] bool ready() const;
  [[nodiscard]] brpc::Channel* channel() const { return channel_.get(); }
  [[nodiscard]] const ClientOptions& options() const { return options_; }

 private:
  const ClientOptions&             options_;
  std::string                      endpoint_;
  std::unique_ptr<brpc::Channel>   channel_;
};

/**
 * 拼 HTTP URI：`http://<endpoint>/v1/objects/<bucket>/<key>`。
 * 对 bucket 整段、key 中除 '/' 之外的字符做 percent-encode，与 server 端
 * gateway/src/api/http_frontend.cpp::ParseObjectPath 的约定一致（key 允许含 /）。
 */
[[nodiscard]] std::string
  BuildObjectUri(const std::string& endpoint, const std::string& bucket,
                 const std::string& key);

}  // namespace us3_turbo_access::client
