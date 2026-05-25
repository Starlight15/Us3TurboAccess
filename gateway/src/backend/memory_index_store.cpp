#include "backend/memory_index_store.h"

#include <chrono>
#include <functional>
#include <sstream>
#include <utility>

#include "common/error.h"

namespace us3_turbo_access::gateway::backend {

std::string MemoryIndexStore::MakeKey(std::string_view bucket,
                                       std::string_view key) {
  std::string out;
  out.reserve(bucket.size() + 1U + key.size());
  out.append(bucket);
  out.push_back('/');
  out.append(key);
  return out;
}

MemoryIndexStore::ObjectShard& MemoryIndexStore::ShardFor(const std::string& id) {
  return shards_[std::hash<std::string>{}(id) % kShardCount];
}

Result<ObjectMetadata> MemoryIndexStore::Head(std::string_view bucket,
                                                std::string_view key) {
  const auto id = MakeKey(bucket, key);
  auto& shard = ShardFor(id);
  std::scoped_lock lock(shard.mu);
  auto it = shard.entries.find(id);
  if (it == shard.entries.end()) {
    return Result<ObjectMetadata>::Failure(
        common::MakeError(ErrorCode::kNotFound, "object not found"));
  }
  return Result<ObjectMetadata>::Success(it->second);
}

Result<bool> MemoryIndexStore::PutObjectIndex(std::string_view bucket,
                                                std::string_view key,
                                                std::size_t content_length,
                                                std::string_view etag,
                                                std::string_view version) {
  const auto id = MakeKey(bucket, key);
  auto& shard = ShardFor(id);
  std::scoped_lock lock(shard.mu);
  auto& meta = shard.entries[id];
  meta.content_length = content_length;
  meta.etag = std::string(etag);
  meta.version = std::string(version);
  return Result<bool>::Success(true);
}

Result<bool> MemoryIndexStore::DeleteObjectIndex(std::string_view bucket,
                                                   std::string_view key) {
  const auto id = MakeKey(bucket, key);
  auto& shard = ShardFor(id);
  std::scoped_lock lock(shard.mu);
  const bool erased = shard.entries.erase(id) > 0;
  return Result<bool>::Success(erased);
}

Result<std::string> MemoryIndexStore::StartMultipart(
    std::string_view bucket, std::string_view key,
    std::optional<std::size_t> total_size_hint) {
  const auto seq = mpu_seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
  std::ostringstream oss;
  oss << "mpu-" << std::hex << seq << '-'
      << std::chrono::steady_clock::now().time_since_epoch().count();
  std::string upload_id = oss.str();

  MpuRecord rec;
  rec.bucket = std::string(bucket);
  rec.key = std::string(key);
  rec.total_size_hint = total_size_hint;

  std::scoped_lock lock(mpu_mu_);
  mpu_.emplace(upload_id, std::move(rec));
  return Result<std::string>::Success(std::move(upload_id));
}

Result<bool> MemoryIndexStore::AbortMultipart(std::string_view upload_id) {
  std::scoped_lock lock(mpu_mu_);
  const bool erased = mpu_.erase(std::string(upload_id)) > 0;
  return Result<bool>::Success(erased);
}

Result<ObjectMetadata> MemoryIndexStore::CommitMultipart(
    std::string_view upload_id, std::string_view bucket, std::string_view key,
    std::size_t content_length, std::string_view etag,
    std::string_view version) {
  // 验证 upload_id 存在；同时把 mpu 记录清掉。
  {
    std::scoped_lock lock(mpu_mu_);
    if (mpu_.erase(std::string(upload_id)) == 0) {
      return Result<ObjectMetadata>::Failure(common::MakeError(
          ErrorCode::kNotFound, "multipart upload_id not found in index"));
    }
  }
  // 登记最终对象索引。
  ObjectMetadata meta;
  meta.content_length = content_length;
  meta.etag = std::string(etag);
  meta.version = std::string(version);
  const auto id = MakeKey(bucket, key);
  auto& shard = ShardFor(id);
  std::scoped_lock lock(shard.mu);
  shard.entries[id] = meta;
  return Result<ObjectMetadata>::Success(std::move(meta));
}

}  // namespace us3_turbo_access::gateway::backend
