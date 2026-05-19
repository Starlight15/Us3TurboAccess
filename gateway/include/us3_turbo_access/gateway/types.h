#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace us3_turbo_access::gateway {

/**
 * @brief Object operations recognised by the gateway.
 */
enum class OperationType {
  kGet,
  kPut,
  kHead,
};

/**
 * @brief Wire identifier of the data path negotiated with the client.
 *
 * Values mirror the strings the client SDK sends on the wire
 * (`http-tcp`, `native-rdma`, `gds-cuobject`).
 */
enum class DataPath {
  kHttpTcp,
  kNativeRdma,
  kGdsCuObject,
};

[[nodiscard]] std::string_view ToString(OperationType op) noexcept;
[[nodiscard]] std::string_view ToString(DataPath path) noexcept;
[[nodiscard]] DataPath          ParseDataPath(std::string_view text) noexcept;
[[nodiscard]] OperationType     ParseOperationType(std::string_view text) noexcept;

/**
 * @brief Plain-old metadata describing a stored object.
 */
struct ObjectMetadata {
  std::size_t content_length{0};
  std::string etag;
  std::string version;
};

/**
 * @brief Single chunk descriptor inside a negotiated transfer plan.
 */
struct ChunkPlanItem {
  std::uint64_t offset{0};
  std::uint64_t size{0};
};

}  // namespace us3_turbo_access::gateway
