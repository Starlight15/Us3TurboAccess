#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/logger.h>

#include "backend/backend.h"
#include "core/multipart/multipart_coordinator.h"
#include "us3_turbo_access/gateway/result.h"
#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway::core::multipart {

/**
 * @brief Application-service layer for multipart control-plane operations.
 *
 * This class is the single entry point for StartUpload / CompleteUpload /
 * AbortUpload across all API surfaces (baidu_std RPC, HTTP, etc.).
 *
 * Why this layer exists (round 1):
 *   Previously, ControlPlaneService and HttpFrontend each called
 *   MultipartCoordinator directly, which meant the API layer was doing
 *   both protocol adaptation *and* business orchestration.
 *   MultipartAppService absorbs the orchestration concern so that API
 *   handlers remain pure protocol adapters (decode → call → encode).
 *
 * What this layer does NOT do (round 1):
 *   - UploadPart — left in the data-path executors; will be pulled in
 *     a future round.
 *   - GDS token / UCX write_done / HTTP body — these are data-plane
 *     concerns that stay in their respective executors.
 */
class MultipartAppService {
 public:
  MultipartAppService(MultipartCoordinator& coordinator,
                      std::shared_ptr<spdlog::logger> logger);

  [[nodiscard]] Result<StartUploadResult>
    StartUpload(const StartUploadParams& params);

  [[nodiscard]] Result<ObjectMetadata>
    CompleteUpload(std::string_view upload_id,
                   const std::vector<backend::PartRecord>& parts,
                   std::string_view expected_data_path);

  [[nodiscard]] Result<bool>
    AbortUpload(std::string_view upload_id,
                std::string_view expected_data_path);

 private:
  MultipartCoordinator&             coordinator_;
  std::shared_ptr<spdlog::logger>   logger_;
};

}  // namespace us3_turbo_access::gateway::core::multipart
