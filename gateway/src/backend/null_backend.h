#pragma once

#include "backend/backend.h"

namespace us3_turbo_access::gateway::backend {

/**
 * @brief Backend that discards writes and reports zero-byte reads.
 *
 * Intended for protocol-only benchmarks and tests where object content is
 * irrelevant.
 */
class NullBackend final : public IBackend {
 public:
  [[nodiscard]] std::string_view kind() const noexcept override { return "null"; }

  [[nodiscard]] Result<ObjectMetadata>
    Head(std::string_view bucket, std::string_view key) override;

  [[nodiscard]] Result<std::size_t>
    Read(std::string_view bucket, std::string_view key,
         std::uint64_t offset, std::span<std::byte> dst) override;

  [[nodiscard]] Result<ObjectMetadata>
    Write(std::string_view bucket, std::string_view key,
          std::span<const std::byte> src) override;

  [[nodiscard]] Result<ObjectMetadata>
    WriteRange(std::string_view bucket, std::string_view key,
               std::uint64_t offset, std::span<const std::byte> src,
               std::optional<std::size_t> total_size) override;
};

}  // namespace us3_turbo_access::gateway::backend
