#include "client/src/transports/gds/cuobject_client.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "client/src/core/common/errors.h"
#include "client/src/data/gds_data_client.h"
#include "client/src/transports/gds/chunk_dispatcher.h"
#include "client/src/transports/gds/gds_memory_registry.h"

namespace us3_turbo_access::client {
namespace {

template <typename BufferPointer>
[[nodiscard]] Result<TransferOutcome> ExecuteTransferGet(
    const ClientOptions& options, const GdsDataClient& data_client,
    const TransferSession& session, const GetObjectRequest& request,
    BufferPointer buffer, std::size_t buffer_size,
    const std::string& upload_id = {}, std::uint32_t part_number = 0) {
  if (buffer == nullptr || buffer_size == 0U) {
    return Result<TransferOutcome>::Failure(MakeInvalidArgument(
        "GDS transfer requires non-null buffer and positive size"));
  }

  const auto req_bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
      request.length.value_or(buffer_size), buffer_size));
  if (req_bytes == 0U) {
    return Result<TransferOutcome>::Failure(MakeInvalidArgument(
        "Requested transfer length is zero"));
  }

  GdsMemoryRegistry registry;
  auto tok = registry.AcquireToken(buffer, req_bytes, 0, OperationType::kGet);
  if (!tok.success()) return Result<TransferOutcome>::Failure(tok.error());

  ChunkDispatcher dispatcher(options, data_client, session, request, OperationType::kGet);
  if (!upload_id.empty()) {
    dispatcher.SetMultipart(upload_id, part_number);
  }
  auto disp = dispatcher.Dispatch(std::string(tok.value().str()),
                                   request.offset, req_bytes);
  if (!disp.success()) return Result<TransferOutcome>::Failure(disp.error());

  TransferOutcome outcome;
  outcome.selected_flow     = DataFlow::GPUDirect;
  outcome.request_id        = session.meta.request_id;
  outcome.session_id        = session.meta.session_id;
  outcome.gateway_id        = disp.value().gateway_id.empty()
                                  ? session.meta.gateway_id
                                  : disp.value().gateway_id;
  outcome.transfer_status   = disp.value().transfer_status.empty()
                                  ? std::string{"completed"}
                                  : disp.value().transfer_status;
  outcome.rdma_reply        = disp.value().rdma_reply;
  outcome.etag              = disp.value().etag;
  outcome.version           = disp.value().version;
  outcome.bytes_transferred = req_bytes;
  if (disp.value().crc32c != 0) {
    outcome.server_crc32c = disp.value().crc32c;
  }

  if (request.progress_callback) {
    request.progress_callback({.bytes_completed = req_bytes,
                                .bytes_total = req_bytes,
                                .data_flow = DataFlow::GPUDirect});
  }
  return Result<TransferOutcome>::Success(std::move(outcome));
}

template <typename BufferPointer>
[[nodiscard]] Result<TransferOutcome> ExecuteTransferPut(
    const ClientOptions& options, const GdsDataClient& data_client,
    const TransferSession& session, const PutObjectRequest& request,
    BufferPointer buffer, std::size_t buffer_size,
    const std::string& upload_id = {}, std::uint32_t part_number = 0) {
  if (buffer == nullptr || buffer_size == 0U) {
    return Result<TransferOutcome>::Failure(MakeInvalidArgument(
        "GDS transfer requires non-null buffer and positive size"));
  }

  GdsMemoryRegistry registry;
  auto tok = registry.AcquireToken(buffer, buffer_size, 0, OperationType::kPut);
  if (!tok.success()) return Result<TransferOutcome>::Failure(tok.error());

  ChunkDispatcher dispatcher(options, data_client, session, request, OperationType::kPut);
  if (!upload_id.empty()) {
    dispatcher.SetMultipart(upload_id, part_number);
  }
  auto disp = dispatcher.Dispatch(std::string(tok.value().str()), 0, buffer_size);
  if (!disp.success()) return Result<TransferOutcome>::Failure(disp.error());

  TransferOutcome outcome;
  outcome.selected_flow     = DataFlow::GPUDirect;
  outcome.request_id        = session.meta.request_id;
  outcome.session_id        = session.meta.session_id;
  outcome.gateway_id        = disp.value().gateway_id.empty()
                                  ? session.meta.gateway_id
                                  : disp.value().gateway_id;
  outcome.transfer_status   = disp.value().transfer_status.empty()
                                  ? std::string{"completed"}
                                  : disp.value().transfer_status;
  outcome.rdma_reply        = disp.value().rdma_reply;
  outcome.etag              = disp.value().etag;
  outcome.version           = disp.value().version;
  outcome.bytes_transferred = buffer_size;
  if (disp.value().crc32c != 0) {
    outcome.server_crc32c = disp.value().crc32c;
  }

  if (request.progress_callback) {
    request.progress_callback({.bytes_completed = buffer_size,
                                .bytes_total = buffer_size,
                                .data_flow = DataFlow::GPUDirect});
  }
  return Result<TransferOutcome>::Success(std::move(outcome));
}

}  // namespace

Result<TransferOutcome> CuObjectClient::ExecuteGet(
    const ClientOptions& options, const GdsDataClient& data_client,
    const TransferSession& session, const GetObjectRequest& request,
    MutableBufferView buffer) const {
  return ExecuteTransferGet(options, data_client, session, request, buffer.data,
                         buffer.size);
}

Result<TransferOutcome> CuObjectClient::ExecutePut(
    const ClientOptions& options, const GdsDataClient& data_client,
    const TransferSession& session, const PutObjectRequest& request,
    ConstBufferView buffer) const {
  return ExecuteTransferPut(options, data_client, session, request, buffer.data,
                         buffer.size);
}

Result<TransferOutcome> CuObjectClient::ExecutePutPart(
    const ClientOptions& options, const GdsDataClient& data_client,
    const TransferSession& session, const PutObjectRequest& request,
    ConstBufferView buffer, const std::string& upload_id,
    std::uint32_t part_number) const {
  return ExecuteTransferPut(options, data_client, session, request, buffer.data,
                         buffer.size, upload_id, part_number);
}

}  // namespace us3_turbo_access::client
