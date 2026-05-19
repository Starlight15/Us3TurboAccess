#include "client/src/transports/gds/chunk_dispatcher.h"

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {
namespace {

[[nodiscard]] Error MakeMissingRdmaTokenError(const std::string& request_id) {
  return MakeTransportFailure("cuObject callback did not provide a valid RDMA token",
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

Result<ChunkDispatcher::Outcome> ChunkDispatcher::Dispatch(const std::string& rdma_token,
                                                            std::uint64_t chunk_offset,
                                                            std::size_t chunk_size) const {
  if (rdma_token.empty()) {
    return Result<Outcome>::Failure(MakeMissingRdmaTokenError(request_id_));
  }

  ChunkTransferRequest chunk_req = BuildChunkRequest(options_, ChunkRpcInput{
      .operation = op_,
      .request = request_,
      .buffer_type = BufferType::kCudaDevice,
      .path = DataPath::kGdsCuObject,
      .request_id = request_id_,
      .session_id = session_id_,
      .ticket = ticket_,
      .rdma_token = rdma_token,
      .chunk_offset = chunk_offset,
      .chunk_size = chunk_size,
  });

  auto response = data_client_.ExecuteGdsChunk(chunk_req);
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
