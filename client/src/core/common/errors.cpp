#include "client/src/core/common/errors.h"

#include <brpc/errno.pb.h>

namespace us3_turbo_access::client {

Error MakeNotInitialized(std::string_view component) {
  return MakeError(ErrorCode::kInternal,
                   std::string(component) + " is not initialized. Call Client::Initialize first.",
                   true);
}

Error MakeInvalidArgument(std::string_view message) {
  return MakeError(ErrorCode::kInvalidArgument, std::string(message));
}

Error MakeUnsupportedPath(DataPath path, std::string_view message) {
  return MakeError(ErrorCode::kUnsupported, std::string(message), false,
                   std::string(ToString(path)));
}

Error MakeRpcFailure(const brpc::Controller& controller,
                     std::string_view message,
                     DataPath path,
                     std::string_view request_id) {
  // brpc 超时 (ERPCTIMEDOUT / ETIMEDOUT) → 统一映射为 kTimeout 让上层
  // RetryIfRetryable 能正确处理（kTimeout 默认 retryable=true）。
  const int err = controller.ErrorCode();
  const bool is_timeout = (err == brpc::ERPCTIMEDOUT) || (err == ETIMEDOUT);
  return MakeError(is_timeout ? ErrorCode::kTimeout : ErrorCode::kControlPlaneError,
                   std::string(message) + ": " + controller.ErrorText(), true,
                   std::string(ToString(path)), std::string(request_id));
}

Error MakeTransportFailure(std::string_view message,
                           DataPath path,
                           std::string_view request_id,
                           bool retryable) {
  return MakeError(ErrorCode::kTransportError, std::string(message), retryable,
                   std::string(ToString(path)), std::string(request_id));
}

Error MakeRegistrationFailure(std::string_view message) {
  return MakeError(ErrorCode::kRegistrationFailed, std::string(message));
}

}  // namespace us3_turbo_access::client
