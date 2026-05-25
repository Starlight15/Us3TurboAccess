#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace us3_turbo_access::gateway::common {

/**
 * @brief Generates a random identifier in the form "<prefix><16 hex bytes>".
 */
[[nodiscard]] std::string MakeRandomId(std::string_view prefix);

/**
 * @brief Returns an ISO 8601 timestamp representing now + @p ttl.
 */
[[nodiscard]] std::string MakeExpireAt(std::chrono::seconds ttl);

}  // namespace us3_turbo_access::gateway::common
