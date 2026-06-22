#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/multipart/multipart.h"
#include "us3_turbo_access/gateway/result.h"

namespace us3_turbo_access::gateway::core::multipart {

/**
 * @brief Sharded registry of in-flight multipart uploads.
 *
 * upload_id 由 store 在 Create 时生成；后续操作通过 Find 拿 shared_ptr，
 * 调用方负责对返回对象内部的 `mu` 上锁。Sweep 用 last_activity_at 做过期判断。
 */
class MultipartStore {
 public:
  explicit MultipartStore(std::chrono::seconds ttl);

  struct CreateParams {
    std::string                bucket;
    std::string                object_key;
    std::string                data_flow;
    std::optional<std::size_t> expected_total_size;
    std::string                backend_upload_id;
  };

  [[nodiscard]] std::shared_ptr<MultipartUpload> Create(const CreateParams& p);

  [[nodiscard]] std::shared_ptr<MultipartUpload>
    Find(std::string_view upload_id) const;

  void Touch(MultipartUpload& upload,
             std::chrono::steady_clock::time_point now);

  bool Erase(std::string_view upload_id);

  std::size_t SweepExpired(std::chrono::steady_clock::time_point now,
                           std::vector<std::shared_ptr<MultipartUpload>>& out_expired);

  [[nodiscard]] std::chrono::seconds ttl() const { return ttl_; }

 private:
  static constexpr std::size_t kShardCount = 32;

  struct Shard {
    mutable std::mutex                                                  mu;
    std::unordered_map<std::string, std::shared_ptr<MultipartUpload>>   by_id;
  };

  [[nodiscard]] Shard& ShardFor(std::string_view key) const;

  std::chrono::seconds                    ttl_;
  std::atomic<std::uint64_t>              seq_{0};
  mutable std::array<Shard, kShardCount>  shards_{};
};

}  // namespace us3_turbo_access::gateway::core::multipart
