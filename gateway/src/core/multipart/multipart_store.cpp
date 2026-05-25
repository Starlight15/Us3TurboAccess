#include "core/multipart/multipart_store.h"

#include <functional>
#include <sstream>
#include <utility>

namespace us3_turbo_access::gateway::core::multipart {

MultipartStore::MultipartStore(std::chrono::seconds ttl) : ttl_(ttl) {}

MultipartStore::Shard& MultipartStore::ShardFor(std::string_view key) const {
  return shards_[std::hash<std::string_view>{}(key) % kShardCount];
}

std::shared_ptr<MultipartUpload> MultipartStore::Create(const CreateParams& p) {
  const auto seq = seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
  const auto ticks =
      std::chrono::steady_clock::now().time_since_epoch().count();
  std::ostringstream oss;
  oss << "mpu-" << std::hex << seq << '-' << ticks;

  auto upload = std::make_shared<MultipartUpload>();
  upload->upload_id = oss.str();
  upload->bucket = p.bucket;
  upload->object_key = p.object_key;
  upload->data_path = p.data_path;
  upload->expected_total_size = p.expected_total_size;
  upload->backend_upload_id = p.backend_upload_id;
  upload->state = State::kActive;
  upload->created_at = std::chrono::steady_clock::now();
  upload->last_activity_at = upload->created_at;

  auto& shard = ShardFor(upload->upload_id);
  std::scoped_lock lock(shard.mu);
  shard.by_id.emplace(upload->upload_id, upload);
  return upload;
}

std::shared_ptr<MultipartUpload>
MultipartStore::Find(std::string_view upload_id) const {
  auto& shard = ShardFor(upload_id);
  std::scoped_lock lock(shard.mu);
  auto it = shard.by_id.find(std::string(upload_id));
  if (it == shard.by_id.end()) {
    return nullptr;
  }
  return it->second;
}

void MultipartStore::Touch(MultipartUpload& upload,
                           std::chrono::steady_clock::time_point now) {
  upload.last_activity_at = now;
}

bool MultipartStore::Erase(std::string_view upload_id) {
  auto& shard = ShardFor(upload_id);
  std::scoped_lock lock(shard.mu);
  return shard.by_id.erase(std::string(upload_id)) > 0;
}

std::size_t MultipartStore::SweepExpired(
    std::chrono::steady_clock::time_point now,
    std::vector<std::shared_ptr<MultipartUpload>>& out_expired) {
  if (ttl_.count() <= 0) {
    return 0;
  }
  std::size_t removed = 0;
  for (auto& shard : shards_) {
    std::scoped_lock lock(shard.mu);
    for (auto it = shard.by_id.begin(); it != shard.by_id.end(); ) {
      const auto& upload = *it->second;
      std::chrono::steady_clock::time_point last;
      {
        std::scoped_lock ulock(upload.mu);
        last = upload.last_activity_at;
      }
      if (now - last > ttl_) {
        out_expired.push_back(it->second);
        it = shard.by_id.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
  }
  return removed;
}

}  // namespace us3_turbo_access::gateway::core::multipart
