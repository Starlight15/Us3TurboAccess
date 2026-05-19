#include "backend/memory_backend.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <utility>

#include "common/error.h"

namespace us3_turbo_access::gateway::backend {

MemoryBackend::MemoryBackend(std::size_t capacity_bytes)
    : capacity_bytes_(capacity_bytes) {}

std::string MemoryBackend::MakeKey(std::string_view bucket, std::string_view key) {
  std::string out;
  out.reserve(bucket.size() + 1U + key.size());
  out.append(bucket);
  out.push_back('/');
  out.append(key);
  return out;
}

ObjectMetadata MemoryBackend::MakeMeta(std::size_t size) {
  const auto ticks = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  std::ostringstream etag;
  etag << '"' << std::hex << size << '-' << ticks << '"';
  ObjectMetadata meta;
  meta.content_length = size;
  meta.etag = etag.str();
  meta.version = std::to_string(ticks);
  return meta;
}

Result<ObjectMetadata> MemoryBackend::Head(std::string_view bucket,
                                           std::string_view key) {
  const auto id = MakeKey(bucket, key);
  std::scoped_lock lock(mu_);
  auto it = entries_.find(id);
  if (it == entries_.end()) {
    return Result<ObjectMetadata>::Failure(
        common::MakeError(ErrorCode::kNotFound, "object not found"));
  }
  return Result<ObjectMetadata>::Success(it->second.meta);
}

Result<std::size_t> MemoryBackend::Read(std::string_view bucket,
                                        std::string_view key,
                                        std::uint64_t offset,
                                        std::span<std::byte> dst) {
  const auto id = MakeKey(bucket, key);
  std::scoped_lock lock(mu_);
  auto it = entries_.find(id);
  if (it == entries_.end()) {
    return Result<std::size_t>::Failure(
        common::MakeError(ErrorCode::kNotFound, "object not found"));
  }
  const auto& body = it->second.body;
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
  return Result<std::size_t>::Success(to_copy);
}

Result<ObjectMetadata> MemoryBackend::Write(std::string_view bucket,
                                            std::string_view key,
                                            std::span<const std::byte> src) {
  return WriteRange(bucket, key, 0, src, src.size());
}

Result<ObjectMetadata> MemoryBackend::WriteRange(
    std::string_view bucket, std::string_view key, std::uint64_t offset,
    std::span<const std::byte> src, std::optional<std::size_t> total_size) {
  const auto id = MakeKey(bucket, key);
  std::scoped_lock lock(mu_);
  auto& entry = entries_[id];

  const std::size_t off = static_cast<std::size_t>(offset);
  const std::size_t end = off + src.size();
  const std::size_t target =
      std::max(total_size.value_or(end), end);
  const std::size_t old_size = entry.body.size();
  if (target > old_size) {
    const auto growth = target - old_size;
    if (used_bytes_ + growth > capacity_bytes_) {
      if (old_size == 0U) {
        entries_.erase(id);
      }
      return Result<ObjectMetadata>::Failure(common::MakeError(
          ErrorCode::kCapacityExceeded,
          "memory backend capacity exceeded (" + std::to_string(used_bytes_) +
              "/" + std::to_string(capacity_bytes_) + " bytes)"));
    }
    entry.body.resize(target);
    used_bytes_ += growth;
  } else if (target < old_size) {
    entry.body.resize(target);
    used_bytes_ -= (old_size - target);
  }
  if (!src.empty()) {
    std::memcpy(entry.body.data() + off, src.data(), src.size());
  }
  entry.meta = MakeMeta(entry.body.size());
  return Result<ObjectMetadata>::Success(entry.meta);
}

}  // namespace us3_turbo_access::gateway::backend
