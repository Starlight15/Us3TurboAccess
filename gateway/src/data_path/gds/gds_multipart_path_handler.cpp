#include "data_path/gds/gds_multipart_path_handler.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "core/multipart/multipart_coordinator.h"
#include "data_path/gds/gds_executor.h"

namespace us3_turbo_access::gateway::data_flow::gds {

GdsMultipartPathHandler::GdsMultipartPathHandler(
    GdsExecutor& executor,
    core::multipart::MultipartCoordinator& coordinator,
    std::shared_ptr<spdlog::logger> logger)
    : executor_(executor),
      coordinator_(coordinator),
      logger_(std::move(logger)) {}

Result<GdsMultipartPartResult>
GdsMultipartPathHandler::UploadPart(const core::Session& session,
                                    const std::string& rdma_token,
                                    std::string_view upload_id,
                                    std::uint32_t part_number,
                                    std::uint64_t chunk_offset,
                                    std::uint64_t chunk_size,
                                    std::string_view checksum_policy) {
  // 1. Lookup the upload to resolve upload_id → backend_upload_id.
  auto lookup = coordinator_.Lookup(upload_id);
  if (!lookup.success()) {
    return Result<GdsMultipartPartResult>::Failure(lookup.error());
  }
  auto upload = lookup.value();

  // 2. Pull data from client GPU via RDMA and write to backend.
  auto part_etag = executor_.PutPart(
      session, rdma_token, upload->backend_upload_id,
      part_number, chunk_offset, chunk_size, checksum_policy,
      upload.get());
  if (!part_etag.success()) {
    return Result<GdsMultipartPartResult>::Failure(part_etag.error());
  }

  // 3. Register the part with the coordinator for CompleteUpload validation.
  coordinator_.RegisterPart(*upload, part_number,
                            chunk_offset, chunk_size,
                            part_etag.value());

  // 4. Build GDS-specific result.
  GdsMultipartPartResult result;
  result.part_etag  = part_etag.value();
  result.rdma_reply = "gds-cuobject-rdma-read";
  result.crc32c     = 0;
  return Result<GdsMultipartPartResult>::Success(std::move(result));
}

}  // namespace us3_turbo_access::gateway::data_flow::gds
