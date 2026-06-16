#include "data_path/http/http_multipart_path_handler.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <butil/iobuf.h>

#include "common/error.h"
#include "core/multipart/multipart_coordinator.h"

namespace us3_turbo_access::gateway::data_path::http {

HttpMultipartPathHandler::HttpMultipartPathHandler(
    HttpExecutor& executor,
    core::multipart::MultipartCoordinator& coordinator,
    std::shared_ptr<spdlog::logger> logger)
    : executor_(executor),
      coordinator_(coordinator),
      logger_(std::move(logger)) {}

Result<TransferReport>
HttpMultipartPathHandler::UploadPart(std::string_view upload_id,
                                     std::uint32_t part_number,
                                     const butil::IOBuf& body,
                                     std::optional<std::uint32_t> expected_crc32c) {
  // 1. Lookup the upload to resolve upload_id → backend_upload_id.
  auto lookup = coordinator_.Lookup(upload_id);
  if (!lookup.success()) {
    return Result<TransferReport>::Failure(lookup.error());
  }
  auto upload = lookup.value();

  // 2. Write part data to backend (CRC + write, no multipart semantics).
  auto write_result = executor_.WritePartData(
      upload->backend_upload_id, part_number, body, expected_crc32c);
  if (!write_result.success()) {
    return Result<TransferReport>::Failure(write_result.error());
  }

  // 3. Register the part with the coordinator for CompleteUpload validation.
  const std::string part_etag =
      core::multipart::MultipartCoordinator::GeneratePartEtag(
          part_number, body.size());
  coordinator_.RegisterPart(*upload, part_number,
                            /*offset=*/0,
                            static_cast<std::uint64_t>(body.size()),
                            part_etag);

  // 4. Build report — replace the backend etag with the standardised part etag
  //    so that CompleteUpload can cross-check consistently.
  TransferReport report = write_result.value();
  report.meta.etag = part_etag;
  return Result<TransferReport>::Success(std::move(report));
}

}  // namespace us3_turbo_access::gateway::data_path::http
