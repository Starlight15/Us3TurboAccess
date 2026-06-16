#include "data_path/ucx/ucx_multipart_path_handler.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "common/error.h"
#include "core/multipart/multipart_coordinator.h"
#include "data_path/ucx/ucx_executor.h"

namespace us3_turbo_access::gateway::data_path::ucx {

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

  // 2. Delegate data persistence to UcxExecutor.
  //    The executor handles the async write_done protocol and backend write.
  //    When it completes, we register the part and forward the result.
  auto sync = executor_.CommitPartDataAsync(
      session_id, upload->backend_upload_id, part_number,
      bytes_transferred, client_crc32c_b64,
      [this, upload, part_number, bytes_transferred,
       on_done = std::move(on_done)](Result<std::string> write_result) mutable {
        if (!write_result.success()) {
          on_done(Result<UcxMultipartPartResult>::Failure(write_result.error()));
          return;
        }
        // 3. Register the part with the coordinator for CompleteUpload validation.
        const std::string& part_etag = write_result.value();
        coordinator_.RegisterPart(*upload, part_number,
                                  /*offset=*/0,
                                  static_cast<std::uint64_t>(bytes_transferred),
                                  part_etag);

        // 4. Forward result.
        UcxMultipartPartResult result;
        result.part_etag = part_etag;
        on_done(Result<UcxMultipartPartResult>::Success(std::move(result)));
      });

  return sync;
}

}  // namespace us3_turbo_access::gateway::data_path::ucx
