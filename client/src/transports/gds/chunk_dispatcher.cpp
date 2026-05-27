#include "client/src/transports/gds/chunk_dispatcher.h"

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {
namespace {

[[nodiscard]] Error MakeMissingRdmaTokenError(const std::string& request_id) {
  return MakeTransportFailure("GDS chunk dispatched without a valid RDMA token",
                              DataPath::kGdsCuObject, request_id, true);
}

}  // namespace

ChunkDispatcher::ChunkDispatcher(const ClientOptions& options, const GdsDataClient& data_client,
                                 const TransferSession& session, const RequestOptions& request,
                                 OperationType op)
    : data_client_(data_client),
      options_(options),
      op_(op),
      request_(request),
      request_id_(session.meta.request_id),
      session_id_(session.meta.session_id),
      ticket_(session.meta.ticket) {}

// 进入 multipart 模式，记录 part 起点用于将绝对偏移转为 part 内偏移。
void ChunkDispatcher::SetMultipart(std::string upload_id,
                                   std::uint32_t part_number) {
  upload_id_ = std::move(upload_id);
  part_number_ = part_number;
  part_base_offset_ = request_.offset;
}

// 把 (token, offset, size) 打成一发 GdsChunk RPC 发给 gateway，gateway 完成
// libibverbs RDMA 后响应。multipart 模式下绝对 offset 转 part 内偏移。
Result<ChunkDispatcher::Outcome> ChunkDispatcher::Dispatch(const std::string& rdma_token,
                                                            std::uint64_t chunk_offset,
                                                            std::size_t chunk_size) const {
  if (rdma_token.empty()) {
    return Result<Outcome>::Failure(MakeMissingRdmaTokenError(request_id_));
  }

  ChunkOp chunk_req = MakeChunkOp(options_, ChunkOpPlan{
      .operation = op_,
      .request = request_,
      .buffer_type = BufferType::kCudaDevice,
      .path = DataPath::kGdsCuObject,
      .request_id = request_id_,
      .session_id = session_id_,
      .ticket = ticket_,
      .rdma_token = rdma_token,
      .chunk_offset = upload_id_.empty()
                        ? chunk_offset                       // 单对象：绝对偏移
                        : chunk_offset - part_base_offset_,  // multipart：part 内偏移
      .chunk_size = chunk_size,
      .upload_id = upload_id_,
      .part_number = part_number_,
  });

  auto response = data_client_.GdsChunk(chunk_req);
  if (!response.success()) {
    return Result<Outcome>::Failure(response.error());
  }

  return Result<Outcome>::Success(Outcome{
      .gateway_id = response.value().selected_gateway(),
      .transfer_status = response.value().transfer_status(),
      .rdma_reply = response.value().rdma_reply(),
      .etag = response.value().etag(),
      .version = response.value().version(),
  });
}

}  // namespace us3_turbo_access::client
