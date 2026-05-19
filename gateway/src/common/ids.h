#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway::common {

/**
 * @brief Generates a random identifier in the form "<prefix><16 hex bytes>".
 */
[[nodiscard]] std::string MakeRandomId(std::string_view prefix);

/**
 * @brief Returns an ISO 8601 timestamp representing now + @p ttl.
 */
[[nodiscard]] std::string MakeExpireAt(std::chrono::seconds ttl);

/**
 * @brief Builds a chunk plan covering [offset, offset + total_size).
 */
[[nodiscard]] std::vector<ChunkPlanItem>
BuildChunkPlan(std::uint64_t offset, std::uint64_t total_size,
               std::size_t chunk_size_hint);

}  // namespace us3_turbo_access::gateway::common
