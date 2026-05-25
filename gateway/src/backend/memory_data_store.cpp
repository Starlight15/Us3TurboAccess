#include "backend/memory_data_store.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <utility>

#include "common/error.h"
#include "common/metrics.h"

namespace us3_turbo_access::gateway::backend {

MemoryDataStore::MemoryDataStore(std::size_t capacity_bytes)
    : capacity_bytes_(capacity_bytes) {}

std::string MemoryDataStore::MakeKey(std::string_view bucket,
                                      std::string_view key) {
  std::string out;
  out.reserve(bucket.size() + 1U + key.size());
  out.append(bucket);
  out.push_back('/');
  out.append(key);
  return out;
}

MemoryDataStore::ObjectShard& MemoryDataStore::ShardFor(const std::string& id) {
  return shards_[std::hash<std::string>{}(id) % kShardCount];
}

bool MemoryDataStore::ReserveBytes(std::size_t growth) {
  if (growth == 0) {
    return true;
  }
  std::size_t current = used_bytes_.load(std::memory_order_acquire);
  while (true) {
    const std::size_t target = current + growth;
    if (target > capacity_bytes_) {
      return false;
    }
    if (used_bytes_.compare_exchange_weak(current, target,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
      return true;
    }
  }
}

void MemoryDataStore::ReleaseBytes(std::size_t bytes) noexcept {
  if (bytes == 0) {
    return;
  }
  used_bytes_.fetch_sub(bytes, std::memory_order_acq_rel);
}

// 预占对象空间到 total_size，单次扩容到位避免多 chunk resize。
Result<bool> MemoryDataStore::Reserve(std::string_view bucket,
                                       std::string_view key,
                                       std::size_t total_size) {
  const auto id = MakeKey(bucket, key);
  auto& shard = ShardFor(id);
  std::scoped_lock lock(shard.mu);
  auto& body = shard.bodies[id];
  const std::size_t old_size = body.size();
  if (total_size > old_size) {
    if (!ReserveBytes(total_size - old_size)) {
      if (old_size == 0U) {
        shard.bodies.erase(id);
      }
      return Result<bool>::Failure(common::MakeError(
          ErrorCode::kCapacityExceeded, "memory data store capacity exceeded"));
    }
    body.resize(total_size);
  } else if (total_size < old_size) {
    body.resize(total_size);
    ReleaseBytes(old_size - total_size);
  }
  return Result<bool>::Success(true);
}

// 在 offset 处写入 src；total_size 触发预扩容到对象总大小。
Result<bool> MemoryDataStore::WriteRange(std::string_view bucket,
                                          std::string_view key,
                                          std::uint64_t offset,
                                          std::span<const std::byte> src,
                                          std::optional<std::size_t> total_size) {
  const auto id = MakeKey(bucket, key);
  auto& shard = ShardFor(id);
  std::scoped_lock lock(shard.mu);
  auto& body = shard.bodies[id];

  const std::size_t off = static_cast<std::size_t>(offset);
  const std::size_t end = off + src.size();
  const std::size_t target = std::max(total_size.value_or(end), end);
  const std::size_t old_size = body.size();

  if (target > old_size) {
    if (!ReserveBytes(target - old_size)) {
      if (old_size == 0U) {
        shard.bodies.erase(id);
      }
      return Result<bool>::Failure(common::MakeError(
          ErrorCode::kCapacityExceeded, "memory data store capacity exceeded"));
    }
    body.resize(target);
  } else if (target < old_size) {
    body.resize(target);
    ReleaseBytes(old_size - target);
  }
  if (!src.empty()) {
    std::memcpy(body.data() + off, src.data(), src.size());
  }
  common::metrics().backend_write_total << 1;
  common::metrics().backend_write_bytes << static_cast<std::int64_t>(src.size());
  return Result<bool>::Success(true);
}

// 从 offset 起读到 dst；返回实际拷贝的字节数。
Result<std::size_t> MemoryDataStore::Read(std::string_view bucket,
                                            std::string_view key,
                                            std::uint64_t offset,
                                            std::span<std::byte> dst) {
  const auto id = MakeKey(bucket, key);
  auto& shard = ShardFor(id);
  std::scoped_lock lock(shard.mu);
  auto it = shard.bodies.find(id);
  if (it == shard.bodies.end()) {
    return Result<std::size_t>::Failure(
        common::MakeError(ErrorCode::kNotFound, "object data not found"));
  }
  const auto& body = it->second;
  if (offset > body.size()) {
    return Result<std::size_t>::Failure(common::MakeError(
        ErrorCode::kRangeNotSatisfiable, "offset beyond object size"));
  }
  const auto available = body.size() - static_cast<std::size_t>(offset);
  const auto to_copy = std::min(available, dst.size());
  if (to_copy != 0U) {
    std::memcpy(dst.data(), body.data() + static_cast<std::size_t>(offset),
                to_copy);
  }
  common::metrics().backend_read_total << 1;
  common::metrics().backend_read_bytes << static_cast<std::int64_t>(to_copy);
  return Result<std::size_t>::Success(to_copy);
}

Result<bool> MemoryDataStore::DeleteObject(std::string_view bucket,
                                            std::string_view key) {
  const auto id = MakeKey(bucket, key);
  auto& shard = ShardFor(id);
  std::scoped_lock lock(shard.mu);
  auto it = shard.bodies.find(id);
  if (it == shard.bodies.end()) {
    return Result<bool>::Success(false);
  }
  ReleaseBytes(it->second.size());
  shard.bodies.erase(it);
  return Result<bool>::Success(true);
}

// 在 part 内 offset 处累积写入；同一 part 多 chunk 调用会逐步扩容 part body。
Result<bool> MemoryDataStore::WritePart(std::string_view upload_id,
                                         std::uint32_t part_number,
                                         std::uint64_t offset,
                                         std::span<const std::byte> src) {
  std::scoped_lock lock(mpu_mu_);
  auto& mp = mpu_[std::string(upload_id)];  // 首次自动创建
  auto& part = mp.parts[part_number];
  const std::size_t off = static_cast<std::size_t>(offset);
  const std::size_t end = off + src.size();
  const std::size_t old_size = part.size();
  if (end > old_size) {
    if (!ReserveBytes(end - old_size)) {
      return Result<bool>::Failure(common::MakeError(
          ErrorCode::kCapacityExceeded, "memory data store capacity exceeded"));
    }
    mp.reserved_bytes += (end - old_size);
    part.resize(end);
  }
  if (!src.empty()) {
    std::memcpy(part.data() + off, src.data(), src.size());
  }
  return Result<bool>::Success(true);
}

// 放弃 upload：释放预留容量，清掉 staging 字节。
Result<bool> MemoryDataStore::AbortMultipartData(std::string_view upload_id) {
  std::scoped_lock lock(mpu_mu_);
  auto it = mpu_.find(std::string(upload_id));
  if (it == mpu_.end()) {
    return Result<bool>::Success(false);
  }
  ReleaseBytes(it->second.reserved_bytes);
  mpu_.erase(it);
  return Result<bool>::Success(true);
}

// 按 part_number 顺序拼接所有 part，存到 (dest_bucket, dest_key)；释放 staging 容量。
Result<std::size_t> MemoryDataStore::AssembleObject(
    std::string_view upload_id, std::string_view dest_bucket,
    std::string_view dest_key,
    const std::vector<std::uint32_t>& ordered_part_numbers) {
  // 取出 staging 数据并从 multipart 表移除。
  MpuParts mp;
  {
    std::scoped_lock lock(mpu_mu_);
    auto it = mpu_.find(std::string(upload_id));
    if (it == mpu_.end()) {
      return Result<std::size_t>::Failure(common::MakeError(
          ErrorCode::kNotFound, "upload_id not found in data store"));
    }
    mp = std::move(it->second);
    mpu_.erase(it);
  }

  // 计算总长 + 校验 part 完整。
  std::size_t total = 0;
  for (auto pn : ordered_part_numbers) {
    auto pit = mp.parts.find(pn);
    if (pit == mp.parts.end()) {
      ReleaseBytes(mp.reserved_bytes);
      return Result<std::size_t>::Failure(common::MakeError(
          ErrorCode::kBadRequest, "part_number missing in upload data"));
    }
    total += pit->second.size();
  }

  // 按顺序拼接。
  std::vector<std::byte> assembled;
  assembled.reserve(total);
  for (auto pn : ordered_part_numbers) {
    auto& pb = mp.parts[pn];
    assembled.insert(assembled.end(), pb.begin(), pb.end());
  }

  // 释放 staging 容量。
  ReleaseBytes(mp.reserved_bytes);

  // 写入目标对象 body；容量记账按差值结算。
  const auto id = MakeKey(dest_bucket, dest_key);
  auto& shard = ShardFor(id);
  std::scoped_lock lock(shard.mu);
  auto& body = shard.bodies[id];
  const std::size_t old_size = body.size();
  if (total > old_size) {
    if (!ReserveBytes(total - old_size)) {
      if (old_size == 0U) {
        shard.bodies.erase(id);
      }
      return Result<std::size_t>::Failure(common::MakeError(
          ErrorCode::kCapacityExceeded, "memory data store capacity exceeded"));
    }
  } else if (total < old_size) {
    ReleaseBytes(old_size - total);
  }
  body = std::move(assembled);
  return Result<std::size_t>::Success(total);
}

}  // namespace us3_turbo_access::gateway::backend
