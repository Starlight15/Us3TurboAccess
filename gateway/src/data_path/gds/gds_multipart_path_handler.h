#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <spdlog/logger.h>

#include "us3_turbo_access/gateway/result.h"

namespace us3_turbo_access::gateway::core {
class Session;
}  // namespace us3_turbo_access::gateway::core

namespace us3_turbo_access::gateway::core::multipart {
class MultipartCoordinator;
}  // namespace us3_turbo_access::gateway::core::multipart

namespace us3_turbo_access::gateway::data_flow::gds {
class GdsExecutor;
}  // namespace us3_turbo_access::gateway::data_flow::gds

namespace us3_turbo_access::gateway::data_flow::gds {

/**
 * @brief Result of a GDS multipart part upload.
 *
 * Carries the GDS-specific fields needed by ControlPlaneService to
 * fill the GdsChunkResponse, without coupling to protobuf types.
 */
struct GdsMultipartPartResult {
  std::string part_etag;          ///< Etag returned by backend / GDS executor
  std::string rdma_reply;         ///< Fixed: "gds-cuobject-rdma-read"
  std::uint32_t crc32c{0};        ///< Server-side CRC (currently 0 for GDS PUT)
};

/**
 * @brief GDS-specific multipart part handler.
 *
 * Owns the business orchestration for uploading a single part over GDS:
 *   1. Lookup the upload by upload_id (via MultipartCoordinator)
 *   2. Write part data via GDS RDMA pull + backend write (GdsExecutor::PutPart)
 *   3. Register the part with MultipartCoordinator
 *   4. Return a GdsMultipartPartResult
 *
 * This class is GDS-only — it does not attempt to abstract a generic
 * multipart handler for HTTP or UCX. Those paths remain independently
 * managed in their respective handlers.
 */
class GdsMultipartPathHandler final {
 public:
  GdsMultipartPathHandler(GdsExecutor& executor,
                          core::multipart::MultipartCoordinator& coordinator,
                          std::shared_ptr<spdlog::logger> logger);

  /**
   * @brief Upload a single part for a GDS multipart upload.
   *
   * @param session       The GDS session for this transfer
   * @param rdma_token    RDMA token for cuObjServer
   * @param upload_id     Client-facing upload ID
   * @param part_number   1-based part number
   * @param chunk_offset  Byte offset within the object
   * @param chunk_size    Number of bytes to transfer
   * @param checksum_policy  Checksum policy string (e.g. "md5")
   * @return GdsMultipartPartResult with part etag and GDS response fields.
   */
  [[nodiscard]] Result<GdsMultipartPartResult>
    UploadPart(const core::Session& session,
               const std::string& rdma_token,
               std::string_view upload_id,
               std::uint32_t part_number,
               std::uint64_t chunk_offset,
               std::uint64_t chunk_size,
               std::string_view checksum_policy);

 private:
  GdsExecutor&                                  executor_;
  core::multipart::MultipartCoordinator&        coordinator_;
  std::shared_ptr<spdlog::logger>               logger_;
};

}  // namespace us3_turbo_access::gateway::data_flow::gds
