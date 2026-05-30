#include "common/error.h"

#include <utility>

namespace us3_turbo_access::gateway::common {

Error MakeError(ErrorCode code, std::string message, bool retryable,
                std::string request_id) {
  return Error{.code = code,
               .message = std::move(message),
               .retryable = retryable,
               .request_id = std::move(request_id)};
}

int ToHttpStatus(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kSuccess:             return 200;
    case ErrorCode::kInvalidArgument:     return 400;
    case ErrorCode::kBadRequest:          return 400;
    case ErrorCode::kUnsupported:         return 501;
    case ErrorCode::kNotFound:            return 404;
    case ErrorCode::kSessionNotFound:     return 404;
    case ErrorCode::kRangeNotSatisfiable: return 416;
    case ErrorCode::kPayloadTooLarge:     return 413;
    case ErrorCode::kCapacityExceeded:    return 507;
    case ErrorCode::kStaleState:          return 409;
    case ErrorCode::kTicketInvalid:       return 401;
    case ErrorCode::kBackendUnavailable:  return 502;
    case ErrorCode::kRdmaUnavailable:     return 503;
    case ErrorCode::kMethodNotAllowed:    return 405;
    case ErrorCode::kTimeout:             return 504;
    case ErrorCode::kRpcError:
    case ErrorCode::kSerializationError:
    case ErrorCode::kControlPlaneError:
    case ErrorCode::kRegistrationFailed:
    case ErrorCode::kTransportError:
    case ErrorCode::kInternal:
      return 500;
    // No default: force compile-time check when new ErrorCode is added
  }
  // Fallback for unknown codes (should never reach here if all cases covered)
  return 500;
}

}  // namespace us3_turbo_access::gateway::common
