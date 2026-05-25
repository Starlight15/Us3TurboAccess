#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "backend/index_store.h"

namespace us3_turbo_access::gateway::backend {

/**
 * 进程内元数据索引：(bucket, key) → ObjectMetadata；不存对象字节。
 * 对象索引按 "bucket/key" 分到 32 shard 各上一把 mutex；
 * multipart upload-id 单独一张表 + 一把锁。
 */
class MemoryIndexStore final : public IIndexStore {
 public:
  MemoryIndexStore() = default;

  [[nodiscard]] std::string_view kind() const noexcept override { return "memory-index"; }

  [[nodiscard]] Result<ObjectMetadata>
    Head(std::string_view bucket, std::string_view key) override;

  [[nodiscard]] Result<bool>
    PutObjectIndex(std::string_view bucket, std::string_view key,
                   std::size_t content_length, std::string_view etag,
                   std::string_view version) override;

  [[nodiscard]] Result<bool>
    DeleteObjectIndex(std::string_view bucket, std::string_view key) override;

  [[nodiscard]] Result<std::string>
    StartMultipart(std::string_view bucket, std::string_view key,
                   std::optional<std::size_t> total_size_hint) override;

  [[nodiscard]] Result<bool>
    AbortMultipart(std::string_view upload_id) override;

  [[nodiscard]] Result<ObjectMetadata>
    CommitMultipart(std::string_view upload_id, std::string_view bucket,
                    std::string_view key, std::size_t content_length,
                    std::string_view etag, std::string_view version) override;

 private:
  static constexpr std::size_t kShardCount = 32;

  // 对象索引 shard：key 为 "<bucket>/<key>"。
  struct ObjectShard {
    mutable std::mutex                                mu;
    std::unordered_map<std::string, ObjectMetadata>   entries;
  };

  // multipart 索引条目：仅记录"这个 upload_id 对应哪个对象"。
  struct MpuRecord {
    std::string bucket;
    std::string key;
    std::optional<std::size_t> total_size_hint;
  };

  static std::string MakeKey(std::string_view bucket, std::string_view key);
  ObjectShard& ShardFor(const std::string& id);

  std::array<ObjectShard, kShardCount>             shards_;

  // multipart 协议状态独立一把锁。
  std::mutex                                       mpu_mu_;
  std::unordered_map<std::string, MpuRecord>       mpu_;
  std::atomic<std::uint64_t>                       mpu_seq_{0};
};

}  // namespace us3_turbo_access::gateway::backend
