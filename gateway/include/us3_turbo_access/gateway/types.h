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
 * @brief Wire identifier of the data flow resolved with the client.
 *
 * Values mirror the strings the client SDK sends on the wire
 * (`cpu-direct`, `gpu-direct`).
 */
enum class DataFlow {
  NONE,
  CPUDirect,
  GPUDirect,
};

[[nodiscard]] std::string_view ToString(OperationType op) noexcept;
[[nodiscard]] std::string_view ToString(DataFlow flow) noexcept;
[[nodiscard]] DataFlow          ParseDataFlow(std::string_view text) noexcept;
[[nodiscard]] OperationType     ParseOperationType(std::string_view text) noexcept;

/**
 * @brief Plain-old metadata describing a stored object.
 */
struct ObjectMetadata {
  std::size_t content_length{0};
  std::string etag;
  std::string version;
};

}  // namespace us3_turbo_access::gateway
