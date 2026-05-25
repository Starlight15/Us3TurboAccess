#pragma once

#include "backend/data_store.h"

namespace us3_turbo_access::gateway::backend {

/**
 * 空数据存储：所有写均成功不落盘；Read 一律返回 0 字节（success）。
 */
class NullDataStore final : public IDataStore {
 public:
  NullDataStore() = default;

  [[nodiscard]] std::string_view kind() const noexcept override { return "null-data"; }

  [[nodiscard]] Result<bool>
    Reserve(std::string_view bucket, std::string_view key,
            std::size_t total_size) override;

  [[nodiscard]] Result<bool>
    WriteRange(std::string_view bucket, std::string_view key,
               std::uint64_t offset, std::span<const std::byte> src,
               std::optional<std::size_t> total_size) override;

  [[nodiscard]] Result<std::size_t>
    Read(std::string_view bucket, std::string_view key,
         std::uint64_t offset, std::span<std::byte> dst) override;

  [[nodiscard]] Result<bool>
    DeleteObject(std::string_view bucket, std::string_view key) override;

  [[nodiscard]] Result<bool>
    WritePart(std::string_view upload_id, std::uint32_t part_number,
              std::uint64_t offset, std::span<const std::byte> src) override;

  [[nodiscard]] Result<bool>
    AbortMultipartData(std::string_view upload_id) override;

  [[nodiscard]] Result<std::size_t>
    AssembleObject(std::string_view upload_id, std::string_view dest_bucket,
                   std::string_view dest_key,
                   const std::vector<std::uint32_t>& ordered_part_numbers) override;
};

}  // namespace us3_turbo_access::gateway::backend
