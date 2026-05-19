#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "us3_turbo_access/gateway/result.h"
#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway::backend {

/**
 * @brief Abstract object storage that the gateway delegates to.
 *
 * The first version ships in-memory stubs (`MemoryBackend`, `NullBackend`).
 * The interface is intentionally narrow so the real backend can be plugged in
 * later without disturbing the rest of the gateway.
 */
class IBackend {
 public:
  virtual ~IBackend() = default;

  [[nodiscard]] virtual std::string_view kind() const noexcept = 0;

  /**
   * @brief Returns object metadata, or kNotFound when the key is unknown.
   */
  [[nodiscard]] virtual Result<ObjectMetadata>
    Head(std::string_view bucket, std::string_view key) = 0;

  /**
   * @brief Reads up to `dst.size()` bytes starting at @p offset.
   *
   * @return number of bytes copied into @p dst (may be 0 when the offset is
   *         past the end of the object).
   */
  [[nodiscard]] virtual Result<std::size_t>
    Read(std::string_view bucket, std::string_view key,
         std::uint64_t offset, std::span<std::byte> dst) = 0;

  /**
   * @brief Stores @p src as the full object content (offset = 0).
   *
   * Equivalent to `WriteRange(bucket, key, 0, src, src.size())`.
   */
  [[nodiscard]] virtual Result<ObjectMetadata>
    Write(std::string_view bucket, std::string_view key,
          std::span<const std::byte> src) = 0;

  /**
   * @brief Writes @p src at @p offset, optionally pre-sizing to @p total_size.
   *
   * Used by chunked uploads (GDS / future RDMA). When @p total_size is empty
   * the object grows to fit; when provided, the body is pre-allocated to that
   * size on first write so subsequent chunks can land in place.
   */
  [[nodiscard]] virtual Result<ObjectMetadata>
    WriteRange(std::string_view bucket, std::string_view key,
               std::uint64_t offset, std::span<const std::byte> src,
               std::optional<std::size_t> total_size) = 0;

  /**
   * @brief Reserves @p total_size bytes for @p bucket/@p key without writing.
   *
   * Used by the control plane to pre-allocate the object when a PUT session is
   * negotiated. Default implementation calls `WriteRange` with an empty span.
   */
  [[nodiscard]] virtual Result<ObjectMetadata>
    Reserve(std::string_view bucket, std::string_view key,
            std::size_t total_size) {
    return WriteRange(bucket, key, 0, {}, total_size);
  }
};

}  // namespace us3_turbo_access::gateway::backend
