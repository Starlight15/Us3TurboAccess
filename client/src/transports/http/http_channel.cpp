#include "client/src/transports/http/http_channel.h"

#include <cctype>
#include <cstdio>

#include "client/src/core/common/brpc_channel.h"
#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

namespace {

// RFC 3986 unreserved；额外把 '/' 也算可保留，让对象 key 里的层级原样传过去。
bool IsUriSafe(char c, bool keep_slash) {
  if (std::isalnum(static_cast<unsigned char>(c))) return true;
  switch (c) {
    case '-': case '_': case '.': case '~':
      return true;
    case '/':
      return keep_slash;
    default:
      return false;
  }
}

void AppendEncoded(std::string& out, const std::string& s, bool keep_slash) {
  char buf[4];
  for (char c : s) {
    if (IsUriSafe(c, keep_slash)) {
      out.push_back(c);
    } else {
      std::snprintf(buf, sizeof(buf), "%%%02X",
                    static_cast<unsigned>(static_cast<unsigned char>(c)));
      out.append(buf, 3);
    }
  }
}

}  // namespace

HttpChannel::HttpChannel(const ClientOptions& options) : options_(options) {}

Result<bool> HttpChannel::Initialize() {
  if (ready()) return Result<bool>::Success(true);
  if (options_.endpoint.empty()) {
    return Result<bool>::Failure(MakeInvalidArgument("endpoint must not be empty"));
  }
  endpoint_ = TrimTrailingSlash(options_.endpoint);

  auto channel = std::make_unique<brpc::Channel>();
  brpc::ChannelOptions ch;
  // 关键差异点：跟 BrpcChannel（baidu_std）唯一不一样的就是 protocol。
  ch.protocol           = "http";
  ch.connection_type    = "pooled";  // 复用 TCP 长连接（keep-alive）
  ch.connect_timeout_ms = static_cast<int>(options_.default_timeout.count());
  ch.timeout_ms         = static_cast<int>(options_.default_timeout.count());
  ch.max_retry          = 0;          // V1 不在 brpc 层做重试，留给未来 V2 的 wrapper
  if (channel->Init(endpoint_.c_str(), nullptr, &ch) != 0) {
    return Result<bool>::Failure(MakeError(
        ErrorCode::kRpcError,
        "Failed to initialize brpc HTTP channel: " + endpoint_, true));
  }
  channel_ = std::move(channel);
  return Result<bool>::Success(true);
}

void HttpChannel::Shutdown() {
  channel_.reset();
  endpoint_.clear();
}

bool HttpChannel::ready() const { return channel_ != nullptr; }

std::string BuildObjectUri(const std::string& endpoint,
                            const std::string& bucket,
                            const std::string& key) {
  std::string uri = "http://";
  uri += endpoint;
  uri += "/v1/objects/";
  AppendEncoded(uri, bucket, /*keep_slash=*/false);
  uri.push_back('/');
  AppendEncoded(uri, key, /*keep_slash=*/true);
  return uri;
}

}  // namespace us3_turbo_access::client
