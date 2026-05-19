#pragma once

#include <memory>

#include <spdlog/logger.h>

namespace us3_turbo_access::gateway::common {

/**
 * @brief Returns a non-null logger.
 *
 * If @p logger is null, a default stdout colour logger named "gateway" is
 * created and returned. Subsequent calls receive the same default instance.
 */
[[nodiscard]] std::shared_ptr<spdlog::logger> EnsureLogger(
    std::shared_ptr<spdlog::logger> logger);

}  // namespace us3_turbo_access::gateway::common
