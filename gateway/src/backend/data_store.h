#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "us3_turbo_access/gateway/result.h"

namespace us3_turbo_access::gateway::backend {

/**
 * 数据层：只管字节存储，不感知 etag/version。
 * 真实实现可对接本地 NVMe / 块设备 / 远端存储节点。
 */
class IDataStore {
 public:
  virtual ~IDataStore() = default;

  IDataStore(const IDataStore&) = delete;
  IDataStore& operator=(const IDataStore&) = delete;

  [[nodiscard]] virtual std::string_view kind() const noexcept = 0;

  // 单对象数据 ---------------------------------------------------------
  [[nodiscard]] virtual Result<bool>
    Reserve(std::string_view bucket, std::string_view key,
            std::size_t total_size) = 0;

  [[nodiscard]] virtual Result<bool>
    WriteRange(std::string_view bucket, std::string_view key,
               std::uint64_t offset, std::span<const std::byte> src,
               std::optional<std::size_t> total_size) = 0;

  [[nodiscard]] virtual Result<std::size_t>
    Read(std::string_view bucket, std::string_view key,
         std::uint64_t offset, std::span<std::byte> dst) = 0;

  [[nodiscard]] virtual Result<bool>
    DeleteObject(std::string_view bucket, std::string_view key) = 0;

  // multipart 数据：按 (upload_id, part_number) 寻址 -------------------
  [[nodiscard]] virtual Result<bool>
    WritePart(std::string_view upload_id, std::uint32_t part_number,
              std::uint64_t offset, std::span<const std::byte> src) = 0;

  [[nodiscard]] virtual Result<bool>
    AbortMultipartData(std::string_view upload_id) = 0;

  // 把所有 part 拼接到 (dest_bucket, dest_key) 下成为最终对象；
  // 返回拼接后的总字节数。
  [[nodiscard]] virtual Result<std::size_t>
    AssembleObject(std::string_view upload_id, std::string_view dest_bucket,
                   std::string_view dest_key,
                   const std::vector<std::uint32_t>& ordered_part_numbers) = 0;

 protected:
  IDataStore() = default;
};

}  // namespace us3_turbo_access::gateway::backend
