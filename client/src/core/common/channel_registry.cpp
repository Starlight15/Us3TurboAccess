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

  // GDS 数据面：gds_data_endpoint 非空时另建一条 channel（同 baidu_std 协议/
  // 超时/重试），指向独立 backend 进程；为空时 gds_data_std() 回退到 baidu_
  // (见头文件)，这里不建第二条，保持旧行为。
  if (!options_.gds_data_endpoint.empty()) {
    const std::string gds_endpoint = TrimTrailingSlash(options_.gds_data_endpoint);
    auto gds = std::make_unique<brpc::Channel>();
    brpc::ChannelOptions gds_co = co;  // 与 baidu_ 完全相同的协议/超时/重试
    if (gds->Init(gds_endpoint.c_str(), nullptr, &gds_co) != 0) {
      auto err = MakeError(
          ErrorCode::kRpcError,
          "Failed to initialize gds_data channel: " + gds_endpoint, true);
      last_init_error_ = err;
      return Result<bool>::Failure(std::move(err));
    }
    gds_data_ = std::move(gds);
  }

  last_init_error_.reset();  // 成功了清掉历史错误
  return Result<bool>::Success(true);
}

void ChannelRegistry::Shutdown() {
  gds_data_.reset();
  baidu_.reset();
  endpoint_.clear();
}

bool ChannelRegistry::ready() const { return baidu_ != nullptr; }

}  // namespace us3_turbo_access::client
