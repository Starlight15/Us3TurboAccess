#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "backend/data_store.h"

namespace us3_turbo_access::gateway::backend {

/**
 * 进程内对象字节存储：对象 body + multipart parts。
 * 仅供开发与 CI。对象按 "bucket/key" 分到 32 shard；multipart parts 一张全局表。
 * 全局容量上限由 capacity_bytes_ 约束，used_bytes_ 用 CAS 记账。
 */
class MemoryDataStore final : public IDataStore {
 public:
  explicit MemoryDataStore(std::size_t capacity_bytes);

  [[nodiscard]] std::string_view kind() const noexcept override { return "memory-data"; }

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

 private:
  static constexpr std::size_t kShardCount = 32;

  // 对象 body 分片：key 为 "<bucket>/<key>"。
  struct ObjectShard {
    mutable std::mutex                                       mu;
    std::unordered_map<std::string, std::vector<std::byte>>  bodies;
  };

  // multipart 的 part 数据按 part_number 索引。
  struct MpuParts {
    std::unordered_map<std::uint32_t, std::vector<std::byte>>  parts;
    std::size_t                                                reserved_bytes{0};
  };

  static std::string MakeKey(std::string_view bucket, std::string_view key);
  ObjectShard& ShardFor(const std::string& id);

  // 容量记账：CAS 抢 growth；超容返回 false 不改状态。
  [[nodiscard]] bool ReserveBytes(std::size_t growth);
  void ReleaseBytes(std::size_t bytes) noexcept;

  std::size_t                                              capacity_bytes_;
  std::atomic<std::size_t>                                 used_bytes_{0};
  std::array<ObjectShard, kShardCount>                     shards_;

  // multipart parts 独立一把锁，与对象 shard 解耦。
  std::mutex                                               mpu_mu_;
  std::unordered_map<std::string, MpuParts>                mpu_;
};

}  // namespace us3_turbo_access::gateway::backend
