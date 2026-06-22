#include "client/src/transports/gds/chunk_dispatcher.h"

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {
namespace {

[[nodiscard]] Error MakeMissingRdmaTokenError(const std::string& request_id) {
  return MakeTransportFailure("GDS chunk dispatched without a valid RDMA token",
                              DataFlow::GPUDirect, request_id, true);
}

}  // namespace

ChunkDispatcher::ChunkDispatcher(const ClientOptions& options, const GdsDataClient& data_client,
                                 const TransferSession& session, const GetObjectRequest& request,
                                 OperationType op)
    : data_client_(data_client),
      options_(options),
      op_(op),
      bucket_(request.bucket),
      key_(request.key),
      offset_(request.offset),
      length_(request.length),
      timeout_(request.timeout),
      checksum_policy_(request.checksum_policy),
      extra_headers_(request.extra_headers),
      request_id_(session.meta.request_id),
      session_id_(session.meta.session_id),
      ticket_(session.meta.ticket) {}

ChunkDispatcher::ChunkDispatcher(const ClientOptions& options, const GdsDataClient& data_client,
                                 const TransferSession& session, const PutObjectRequest& request,
                                 OperationType op)
    : data_client_(data_client),
      options_(options),
      op_(op),
      bucket_(request.bucket),
      key_(request.key),
      timeout_(request.timeout),
      checksum_policy_(request.checksum_policy),
      extra_headers_(request.extra_headers),
      request_id_(session.meta.request_id),
      session_id_(session.meta.session_id),
      ticket_(session.meta.ticket) {}

void ChunkDispatcher::SetMultipart(std::string upload_id,
                                   std::uint32_t part_number) {
  upload_id_ = std::move(upload_id);
  part_number_ = part_number;
  part_base_offset_ = offset_;
}

Result<ChunkDispatcher::Outcome> ChunkDispatcher::Dispatch(const std::string& rdma_token,
                                                            std::uint64_t chunk_offset,
                                                            std::size_t chunk_size) const {
  if (rdma_token.empty()) {
    return Result<Outcome>::Failure(MakeMissingRdmaTokenError(request_id_));
  }

  ChunkOp chunk_req = MakeChunkOp(options_, ChunkOpPlan{
      .operation = op_,
      .bucket = bucket_,
      .key = key_,
      .offset = offset_,
      .length = length_,
      .checksum_policy = checksum_policy_,
      .extra_headers = extra_headers_,
      .timeout = timeout_,
      .buffer_type = BufferType::kCudaDevice,
      .path = DataFlow::GPUDirect,
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
      .crc32c = response.value().crc32c(),
  });
}

}  // namespace us3_turbo_access::client
