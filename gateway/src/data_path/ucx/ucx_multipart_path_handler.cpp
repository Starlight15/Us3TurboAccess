#include "data_path/ucx/ucx_multipart_path_handler.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "common/error.h"
#include "core/multipart/multipart_coordinator.h"
#include "core/multipart/multipart.h"
#include "data_path/ucx/ucx_executor.h"

namespace us3_turbo_access::gateway::data_flow::ucx {

UcxMultipartPathHandler::UcxMultipartPathHandler(
    UcxExecutor& executor,
    core::multipart::MultipartCoordinator& coordinator,
    std::shared_ptr<spdlog::logger> logger)
    : executor_(executor),
      coordinator_(coordinator),
      logger_(std::move(logger)) {}

bool UcxMultipartPathHandler::CommitPartAsync(
    std::string_view session_id,
    std::string_view upload_id,
    std::uint32_t part_number,
    std::uint64_t bytes_transferred,
    std::string_view client_crc32c_b64,
    std::function<void(Result<UcxMultipartPartResult>)> on_done) {
  // 1. Lookup the upload to resolve upload_id → backend_upload_id.
  //    This is synchronous (just a hash-table lookup in the store).
  auto lookup = coordinator_.Lookup(upload_id);
  if (!lookup.success()) {
    on_done(Result<UcxMultipartPartResult>::Failure(lookup.error()));
    return true;
  }
  auto upload = lookup.value();

  // 2. Build a self-contained context for the async completion.
  //    The callback does NOT capture `this`, so it is safe even if
  //    this handler is destroyed before write_done fires.
  auto ctx = std::make_shared<CommitPartContext>();
  ctx->coordinator       = &coordinator_;
  ctx->upload            = upload;
  ctx->part_number       = part_number;
  ctx->bytes_transferred = bytes_transferred;
  ctx->on_done           = std::move(on_done);

  // 3. Delegate data persistence to UcxExecutor.
  //    Use std::bind to wire the completion through FinishCommitPart,
  //    which is a free function that does not depend on this handler's
  //    lifetime.
  auto sync = executor_.CommitPartDataAsync(
      session_id, upload->backend_upload_id, part_number,
      bytes_transferred, client_crc32c_b64,
      std::bind(&FinishCommitPart, ctx,
                std::placeholders::_1));

  return sync;
}

// ---- FinishCommitPart: free function, no dependency on handler lifetime ----

void FinishCommitPart(std::shared_ptr<CommitPartContext> ctx,
                      Result<std::string> write_result) {
  if (!write_result.success()) {
    ctx->on_done(Result<UcxMultipartPartResult>::Failure(write_result.error()));
    return;
  }

  // Register the part with the coordinator for CompleteUpload validation.
  const std::string& part_etag = write_result.value();
  ctx->coordinator->RegisterPart(*ctx->upload, ctx->part_number,
                                  /*offset=*/0,
                                  static_cast<std::uint64_t>(ctx->bytes_transferred),
                                  part_etag);

  // Forward result.
  UcxMultipartPartResult result;
  result.part_etag = part_etag;
  ctx->on_done(Result<UcxMultipartPartResult>::Success(std::move(result)));
}

}  // namespace us3_turbo_access::gateway::data_flow::ucx
