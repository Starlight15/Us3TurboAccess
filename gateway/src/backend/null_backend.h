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

  [[nodiscard]] Result<ObjectMetadata>
    Reserve(std::string_view bucket, std::string_view key,
            std::size_t total_size) override;

  [[nodiscard]] Result<std::string>
    StartMultipart(std::string_view bucket, std::string_view key,
                   std::optional<std::size_t> total_size_hint) override;

  [[nodiscard]] Result<std::string>
    WritePart(std::string_view upload_id, std::uint32_t part_number,
              std::uint64_t offset, std::span<const std::byte> src) override;

  [[nodiscard]] Result<ObjectMetadata>
    CompleteMultipart(std::string_view upload_id,
                      const std::vector<PartRecord>& parts) override;

  [[nodiscard]] Result<bool>
    AbortMultipart(std::string_view upload_id) override;
};

}  // namespace us3_turbo_access::gateway::backend
