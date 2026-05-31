#include "client/src/core/common/channel_registry.h"

#include "client/src/core/common/brpc_channel.h"  // TrimTrailingSlash
#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

ChannelRegistry::ChannelRegistry(const ClientOptions& options) : options_(options) {
  // 立即 Initialize 让 baidu_std() 在 ClientCore::Impl 构造完成时就有效；
  // endpoint 为空时延迟到显式 Initialize 调用（baidu_std() 返回 nullptr，
  // 后续三个 baidu_std client 的 Initialize 会因 channel==nullptr 失败）。
  // C.2 后：构造期 Initialize 失败保存到 last_init_error_，让调用方能查
  // 到原始原因（endpoint 无法解析 / brpc Init 返非 0 等）。
  if (!options_.endpoint.empty()) {
    auto r = Initialize();
    if (!r.success()) {
      last_init_error_ = r.error();
    }
  }
}

Result<bool> ChannelRegistry::Initialize() {
  if (ready()) return Result<bool>::Success(true);
  if (options_.endpoint.empty()) {
    auto err = MakeInvalidArgument("endpoint must not be empty");
    last_init_error_ = err;
    return Result<bool>::Failure(std::move(err));
  }

  endpoint_ = TrimTrailingSlash(options_.endpoint);
  auto baidu = std::make_unique<brpc::Channel>();
  brpc::ChannelOptions co;
  co.protocol           = "baidu_std";
  co.connect_timeout_ms = static_cast<int>(options_.default_timeout.count());
  co.timeout_ms         = static_cast<int>(options_.default_timeout.count());
  co.max_retry          = 2;
  if (baidu->Init(endpoint_.c_str(), nullptr, &co) != 0) {
    auto err = MakeError(
        ErrorCode::kRpcError,
        "Failed to initialize shared baidu_std channel: " + endpoint_, true);
    last_init_error_ = err;
    return Result<bool>::Failure(std::move(err));
  }
  baidu_ = std::move(baidu);
  last_init_error_.reset();  // 成功了清掉历史错误
  return Result<bool>::Success(true);
}

void ChannelRegistry::Shutdown() {
  baidu_.reset();
  endpoint_.clear();
}

bool ChannelRegistry::ready() const { return baidu_ != nullptr; }

}  // namespace us3_turbo_access::client
