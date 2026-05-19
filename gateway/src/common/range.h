#pragma once

#include <cstdint>
#include <string>

namespace us3_turbo_access::gateway::common {

/**
 * @brief HTTP Range header descriptor.
 *
 * `offset` is the first byte to return. `length` is how many bytes to return,
 * already clamped against the object size. `partial` is true iff the original
 * request carried a `Range` header.
 */
struct HttpRange {
  std::uint64_t offset{0};
  std::uint64_t length{0};
  bool          partial{false};
  bool          unsatisfiable{false};
};

/**
 * @brief Parses an HTTP `Range` header against an object of @p object_size.
 *
 * Accepts the simple form `bytes=<start>-<end>`. Missing header yields a
 * full-object range. Out-of-range requests return unsatisfiable.
 */
[[nodiscard]] HttpRange ParseHttpRange(const std::string* header_value,
                                       std::uint64_t object_size);

}  // namespace us3_turbo_access::gateway::common
