#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <spdlog/logger.h>

#include "data_path/http/http_executor.h"
#include "us3_turbo_access/gateway/result.h"

namespace butil {
class IOBuf;
}  // namespace butil

namespace us3_turbo_access::gateway::core::multipart {
class MultipartCoordinator;
}  // namespace us3_turbo_access::gateway::core::multipart

namespace us3_turbo_access::gateway::data_path::http {

/**
 * @brief HTTP-specific multipart part handler.
 *
 * Owns the business orchestration for uploading a single part over HTTP:
 *   1. Lookup the upload by upload_id (via MultipartCoordinator)
 *   2. Write part data to the backend (via HttpExecutor::WritePartData)
 *   3. Register the part with MultipartCoordinator
 *   4. Return a TransferReport
 *
 * This class is HTTP-only — it does not attempt to abstract a generic
 * multipart handler for GDS or UCX. Those paths remain independently
 * managed in their respective path handlers (GdsMultipartPathHandler,
 * UcxMultipartPathHandler).
 */
class HttpMultipartPathHandler final {
 public:
  HttpMultipartPathHandler(HttpExecutor& executor,
                           core::multipart::MultipartCoordinator& coordinator,
                           std::shared_ptr<spdlog::logger> logger);

  /**
   * @brief Upload a single part for a multipart upload.
   *
   * @param upload_id    Client-facing upload ID
   * @param part_number  1-based part number
   * @param body         Part payload (IOBuf, zero-copy)
   * @param expected_crc32c  Optional CRC32C the client declared; mismatch
   *                         returns kInvalidArgument.
   * @return TransferReport with part etag, bytes written, and server CRC32C.
   */
  [[nodiscard]] Result<TransferReport>
    UploadPart(std::string_view upload_id,
               std::uint32_t part_number,
               const butil::IOBuf& body,
               std::optional<std::uint32_t> expected_crc32c);

 private:
  HttpExecutor&                                executor_;
  core::multipart::MultipartCoordinator&       coordinator_;
  std::shared_ptr<spdlog::logger>              logger_;
};

}  // namespace us3_turbo_access::gateway::data_path::http
