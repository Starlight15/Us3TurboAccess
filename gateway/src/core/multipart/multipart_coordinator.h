#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/logger.h>

#include "backend/backend.h"
#include "core/multipart/multipart_store.h"
#include "us3_turbo_access/gateway/result.h"
#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway::core::multipart {

struct StartUploadParams {
  std::string                bucket;
  std::string                object_key;
  std::optional<std::size_t> expected_total_size;
  std::string                data_path;
  std::string                idempotency_key;
};

struct StartUploadResult {
  std::string upload_id;
  std::size_t max_part_size{0};
};

/**
 * @brief Bridges control-plane multipart RPCs to backend index ops + the store.
 *
 * Coordinator holds no I/O thread of its own; all work runs on the caller's
 * thread (typically a brpc bthread that already hopped onto the io_pool for
 * heavy work). The coordinator does *not* own data-path executors — the actual
 * RDMA byte transfer for a part is driven by the data-path executor and only
 * the resulting backend etag is fed back via @ref RegisterPart.
 */
class MultipartCoordinator {
 public:
  MultipartCoordinator(backend::IBackend& backend, MultipartStore& store,
                       std::size_t max_part_size,
                       std::size_t min_part_size,
                       std::shared_ptr<spdlog::logger> logger);

  [[nodiscard]] Result<StartUploadResult>
    CreateUpload(const StartUploadParams& params);

  [[nodiscard]] Result<ObjectMetadata>
    CompleteUpload(std::string_view upload_id,
                   const std::vector<backend::PartRecord>& parts,
                   std::string_view expected_data_path);

  [[nodiscard]] Result<bool>
    AbortUpload(std::string_view upload_id,
                std::string_view expected_data_path);

  /**
   * @brief Best-effort backend cleanup for an upload that was already
   *        evicted from the store (e.g. by the sweeper).
   */
  void AbortBackend(std::string_view backend_upload_id);

  /**
   * @brief Mark a part as written: record (part_number, offset, size, etag).
   *
   * Called by the data-path after a successful backend.WritePart. Returns
   * the upload's expected_total_size hint for downstream sizing.
   */
  [[nodiscard]] Result<std::shared_ptr<MultipartUpload>>
    Lookup(std::string_view upload_id) const;

  void RegisterPart(MultipartUpload& upload, std::uint32_t part_number,
                    std::uint64_t offset, std::uint64_t size,
                    std::string etag);

  /**
   * Generate standardized part etag: "{part_number}-{size}".
   * All data paths must use this to ensure etag format consistency.
   */
  static std::string GeneratePartEtag(std::uint32_t part_number, std::uint64_t size);

  [[nodiscard]] std::size_t max_part_size() const noexcept {
    return max_part_size_;
  }

 private:
  backend::IBackend&                backend_;
  MultipartStore&                   store_;
  std::size_t                       max_part_size_;
  std::size_t                       min_part_size_;
  std::shared_ptr<spdlog::logger>   logger_;
};

}  // namespace us3_turbo_access::gateway::core::multipart
