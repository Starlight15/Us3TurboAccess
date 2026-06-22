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
struct MultipartUpload;
}  // namespace us3_turbo_access::gateway::core::multipart

namespace us3_turbo_access::gateway::data_flow::ucx {

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
 * @brief Self-contained context for completing an async multipart part commit.
 *
 * Captures everything needed by the completion callback so that it does NOT
 * depend on UcxMultipartPathHandler being alive. Even if the handler object
 * is destroyed before write_done fires, this context holds its own references
 * and the callback can complete safely.
 */
struct CommitPartContext {
  core::multipart::MultipartCoordinator*        coordinator{nullptr};
  std::shared_ptr<core::multipart::MultipartUpload> upload;
  std::uint32_t                                  part_number{0};
  std::uint64_t                                  bytes_transferred{0};
  std::function<void(Result<UcxMultipartPartResult>)> on_done;
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
 *
 * Lifetime safety:
 *   The async completion callback does NOT capture `this`. Instead it uses
 *   CommitPartContext + FinishCommitPart, which only hold raw pointers to
 *   MultipartCoordinator (guaranteed to outlive the executor's progress
 *   thread) and shared_ptr<MultipartUpload> (reference-counted). So the
 *   UcxMultipartPathHandler object itself can be destroyed at any time
 *   without risking use-after-free in a pending callback.
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

/**
 * @brief Completion helper for CommitPartDataAsync.
 *
 * Called when the executor finishes writing a part to the backend.
 * Registers the part with MultipartCoordinator and forwards the
 * result through the original on_done callback.
 *
 * This is a free function (not a member) so that it does not depend
 * on UcxMultipartPathHandler's lifetime.
 */
void FinishCommitPart(std::shared_ptr<CommitPartContext> ctx,
                      Result<std::string> write_result);

}  // namespace us3_turbo_access::gateway::data_flow::ucx
