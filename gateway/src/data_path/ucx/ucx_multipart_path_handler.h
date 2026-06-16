#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <spdlog/logger.h>

#include "us3_turbo_access/gateway/result.h"

namespace us3_turbo_access::gateway::core::multipart {
class MultipartCoordinator;
}  // namespace us3_turbo_access::gateway::core::multipart

namespace us3_turbo_access::gateway::data_path::ucx {

class UcxExecutor;

/**
 * @brief Result of a UCX multipart part commit.
 *
 * Carries the part etag needed by RdmaDataPlaneService to fill
 * the RdmaCommitPartResponse, without coupling to protobuf types.
 */
struct UcxMultipartPartResult {
  std::string part_etag;
};

/**
 * @brief UCX-specific multipart part handler.
 *
 * Owns the business orchestration for committing a single multipart part
 * over UCX:
 *   1. Lookup the upload by upload_id (via MultipartCoordinator)
 *   2. Delegate data persistence to UcxExecutor::CommitPartDataAsync
 *   3. Register the part with MultipartCoordinator (in the completion callback)
 *   4. Return a UcxMultipartPartResult via the on_done callback
 *
 * This class is UCX-only — it does not attempt to abstract a generic
 * multipart handler for HTTP or GDS. Those paths remain independently
 * managed in their respective handlers.
 *
 * Async contract (mirrors UcxExecutor::CommitObjectAsync):
 *   Returns true  = sync completion (on_done already called).
 *   Returns false = async; on_done will be called on the progress thread
 *                   when write_done arrives.
 */
class UcxMultipartPathHandler final {
 public:
  UcxMultipartPathHandler(UcxExecutor& executor,
                          core::multipart::MultipartCoordinator& coordinator,
                          std::shared_ptr<spdlog::logger> logger);

  /**
   * @brief Commit a multipart part asynchronously.
   *
   * @param session_id        UCX session ID
   * @param upload_id         Client-facing upload ID
   * @param part_number       1-based part number
   * @param bytes_transferred Number of bytes the client wrote
   * @param client_crc32c_b64 Optional base64-encoded CRC32C from client
   * @param on_done           Completion callback; receives UcxMultipartPartResult
   * @return true = sync (on_done called), false = async pending.
   */
  [[nodiscard]] bool
    CommitPartAsync(std::string_view session_id,
                    std::string_view upload_id,
                    std::uint32_t part_number,
                    std::uint64_t bytes_transferred,
                    std::string_view client_crc32c_b64,
                    std::function<void(Result<UcxMultipartPartResult>)> on_done);

 private:
  UcxExecutor&                                  executor_;
  core::multipart::MultipartCoordinator&        coordinator_;
  std::shared_ptr<spdlog::logger>               logger_;
};

}  // namespace us3_turbo_access::gateway::data_path::ucx
