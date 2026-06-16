#include "core/multipart/multipart_app_service.h"

#include <utility>

namespace us3_turbo_access::gateway::core::multipart {

MultipartAppService::MultipartAppService(MultipartCoordinator& coordinator,
                                         std::shared_ptr<spdlog::logger> logger)
    : coordinator_(coordinator),
      logger_(std::move(logger)) {}

Result<StartUploadResult>
MultipartAppService::StartUpload(const StartUploadParams& params) {
  return coordinator_.CreateUpload(params);
}

Result<ObjectMetadata>
MultipartAppService::CompleteUpload(std::string_view upload_id,
                                    const std::vector<backend::PartRecord>& parts,
                                    std::string_view expected_data_path) {
  return coordinator_.CompleteUpload(upload_id, parts, expected_data_path);
}

Result<bool>
MultipartAppService::AbortUpload(std::string_view upload_id,
                                 std::string_view expected_data_path) {
  return coordinator_.AbortUpload(upload_id, expected_data_path);
}

}  // namespace us3_turbo_access::gateway::core::multipart
