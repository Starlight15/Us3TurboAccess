#pragma once

#include <string>

#include "us3_turbo_access/gateway/result.h"

namespace us3_turbo_access::gateway::common {

/**
 * @brief Builds an error tagged with a request id, suitable for logging and
 *        for serialising back to clients.
 */
[[nodiscard]] Error MakeError(ErrorCode code, std::string message,
                              bool retryable = false,
                              std::string request_id = {});

/**
 * @brief Maps an internal ErrorCode to an HTTP status code.
 */
[[nodiscard]] int ToHttpStatus(ErrorCode code) noexcept;

}  // namespace us3_turbo_access::gateway::common
