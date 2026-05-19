#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "backend/backend.h"

namespace us3_turbo_access::gateway::backend {

/**
 * @brief In-memory object backend with a hard capacity ceiling.
 *
 * Suitable for development and CI. Objects are addressed by
 * `"<bucket>/<key>"`. Writes that would exceed the capacity fail with
 * kCapacityExceeded.
 */
class MemoryBackend final : public IBackend {
 public:
  explicit MemoryBackend(std::size_t capacity_bytes);

  [[nodiscard]] std::string_view kind() const noexcept override { return "memory"; }

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

 private:
  struct Entry {
    std::vector<std::byte> body;
    ObjectMetadata         meta;
  };

  static std::string MakeKey(std::string_view bucket, std::string_view key);
  static ObjectMetadata MakeMeta(std::size_t size);

  std::size_t                          capacity_bytes_;
  mutable std::mutex                   mu_;
  std::size_t                          used_bytes_{0};
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace us3_turbo_access::gateway::backend
