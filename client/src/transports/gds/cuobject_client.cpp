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

/* token-direct 数据面入口。
 *
 * 完全绕开 cuObjPut/cuObjGet 同步阻塞机制：
 *   1. GdsMemoryRegistry::AcquireToken 走 cuMemObjGetRDMAToken（PoC 验证
 *      多线程安全），buffer 若未注册先 lazy register 一次。
 *   2. ChunkDispatcher.Dispatch 把 (token, offset, size) 打成一发
 *      GdsChunk RPC 发给 gateway，gateway 端 libibverbs RDMA 完成后响应。
 *   3. RAII 释放 token。
 *
 * 一次 PUT/GET 就是一发 RPC + 一对 token 申请/释放，没有 SDK 内部 chunk
 * 回调循环，没有进程级全局锁；N 个 worker 真正并发。 */
template <typename BufferPointer>
[[nodiscard]] Result<TransferOutcome> ExecuteTransfer(
    const ClientOptions& options, const GdsDataClient& data_client,
    const TransferSession& session, const RequestOptions& request,
    BufferPointer buffer, std::size_t buffer_size, OperationType op,
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

  // 1. AcquireToken：未注册则 lazy register；token RAII 持有
  GdsMemoryRegistry registry;
  auto tok = registry.AcquireToken(buffer, req_bytes, 0, op);
  if (!tok.success()) return Result<TransferOutcome>::Failure(tok.error());

  // 2. 一次 RPC 把 token 送到 gateway，等待 gateway 完成 RDMA 后响应
  ChunkDispatcher dispatcher(options, data_client, session, request, op);
  if (!upload_id.empty()) {
    dispatcher.SetMultipart(upload_id, part_number);
  }
  auto disp = dispatcher.Dispatch(std::string(tok.value().str()),
                                   request.offset, req_bytes);
  if (!disp.success()) return Result<TransferOutcome>::Failure(disp.error());

  // 3. 组装 TransferOutcome；token 在本函数返回时 RAII 释放
  TransferOutcome outcome;
  outcome.selected_path     = DataPath::kGdsCuObject;
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
                                .data_path = DataPath::kGdsCuObject});
  }
  return Result<TransferOutcome>::Success(std::move(outcome));
}

}  // namespace

Result<TransferOutcome> CuObjectClient::ExecuteGet(
    const ClientOptions& options, const GdsDataClient& data_client,
    const TransferSession& session, const RequestOptions& request,
    MutableBufferView buffer) const {
  return ExecuteTransfer(options, data_client, session, request, buffer.data,
                         buffer.size, OperationType::kGet);
}

Result<TransferOutcome> CuObjectClient::ExecutePut(
    const ClientOptions& options, const GdsDataClient& data_client,
    const TransferSession& session, const RequestOptions& request,
    ConstBufferView buffer) const {
  return ExecuteTransfer(options, data_client, session, request, buffer.data,
                         buffer.size, OperationType::kPut);
}

Result<TransferOutcome> CuObjectClient::ExecutePutPart(
    const ClientOptions& options, const GdsDataClient& data_client,
    const TransferSession& session, const RequestOptions& request,
    ConstBufferView buffer, const std::string& upload_id,
    std::uint32_t part_number) const {
  return ExecuteTransfer(options, data_client, session, request, buffer.data,
                         buffer.size, OperationType::kPut, upload_id,
                         part_number);
}

}  // namespace us3_turbo_access::client
