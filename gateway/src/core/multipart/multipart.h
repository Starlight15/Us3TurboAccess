#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace us3_turbo_access::gateway::core::multipart {

enum class State : std::uint8_t {
  kActive     = 0,
  kCompleting = 1,  // claimed by one CompleteUpload caller; in-flight
  kCompleted  = 2,
  kAborted    = 3,
};

struct PartProgress {
  std::uint32_t part_number{0};
  std::uint64_t offset{0};
  std::uint64_t size{0};
  std::string   etag;
};

/**
 * @brief 单次 multipart 上传的运行时状态。
 *
 * Coordinator 通过 MultipartStore 拿到 shared_ptr 后，对成员的修改必须先持有
 * `mu`。`upload_id` / `bucket` / `object_key` 等不可变字段在 Create 之后只读。
 */
struct MultipartUpload {
  std::string                     upload_id;
  std::string                     bucket;
  std::string                     object_key;
  std::string                     data_path;
  std::optional<std::size_t>      expected_total_size;
  std::string                     backend_upload_id;

  mutable std::mutex                              mu;
  std::map<std::uint32_t, PartProgress>           parts;     // ordered by part #
  State                                           state{State::kActive};
  std::chrono::steady_clock::time_point           created_at{};
  std::chrono::steady_clock::time_point           last_activity_at{};
};

}  // namespace us3_turbo_access::gateway::core::multipart
