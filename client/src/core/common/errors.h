#pragma once

#include <string>
#include <string_view>

#include <brpc/controller.h>

#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

/**
 * @brief 客户端各模块共享的错误构造函数。
 *
 * 这些工厂统一了 ErrorCode 选择、retryable 标记和 failed_path / request_id
 * 填充规则，避免各处自己拼。
 */

[[nodiscard]] Error MakeNotInitialized(std::string_view component);
[[nodiscard]] Error MakeInvalidArgument(std::string_view message);
[[nodiscard]] Error MakeUnsupportedPath(DataPath path, std::string_view message);
[[nodiscard]] Error MakeRpcFailure(const brpc::Controller& controller,
                                   std::string_view message,
                                   DataPath path,
                                   std::string_view request_id);
[[nodiscard]] Error MakeTransportFailure(std::string_view message,
                                         DataPath path,
                                         std::string_view request_id,
                                         bool retryable);
[[nodiscard]] Error MakeRegistrationFailure(std::string_view message);

}  // namespace us3_turbo_access::client
