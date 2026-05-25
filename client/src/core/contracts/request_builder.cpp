#include "client/src/core/contracts/request_builder.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <string>

namespace us3_turbo_access::client {
namespace {

constexpr char kRequestIdPrefix[] = "req-";
constexpr char kSessionIdPrefix[] = "ses-";
constexpr char kDefaultGatewayId[] = "gateway-local";

[[nodiscard]] std::string MakeId(std::string_view prefix) {
  static thread_local std::mt19937_64 rng{
      static_cast<std::uint64_t>(std::random_device{}()) ^
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
  const std::uint64_t value = rng();
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016lx", value);
  return std::string(prefix) + buf;
}

[[nodiscard]] RpcCallMetadata MakeRpcContext(const ClientOptions& options,
                                                std::chrono::milliseconds timeout) {
  return RpcCallMetadata{.client_id = options.client_id,
                           .bearer_token = options.bearer_token,
                           .default_headers = options.default_headers,
                           .timeout = timeout};
}

}  // namespace

SessionOpening MakeSessionHandshake(const ClientOptions& options,
                                            const SessionPlan& input) {
  return SessionOpening{
      .context = MakeRpcContext(options, input.request.timeout),
      .operation = input.operation,
      .object = input.request.object,
      .data_path = input.path,
      .buffer_type = input.buffer_type,
      .offset = input.request.offset,
      .length = input.request.length,
      .request_id = MakeId(kRequestIdPrefix),
      .session_id = MakeId(kSessionIdPrefix),
      .idempotency_key = input.request.idempotency_key,
  };
}

ChunkOp MakeChunkOp(const ClientOptions& options, ChunkOpPlan input) {
  ObjectRequest object{
      .object = input.request.object,
      .offset = input.request.offset,
      .length = input.request.length,
      .data_path = input.path,
      .buffer_type = input.buffer_type,
      .checksum_policy = input.request.checksum_policy,
      .extra_headers = input.request.extra_headers,
  };
  return ChunkOp{
      .context = MakeRpcContext(options, input.request.timeout),
      .object = std::move(object),
      .operation = input.operation,
      .request_id = std::move(input.request_id),
      .session_id = std::move(input.session_id),
      .transfer_ticket = std::move(input.ticket),
      .rdma_token = std::move(input.rdma_token),
      .chunk_offset = input.chunk_offset,
      .chunk_size = input.chunk_size,
      .upload_id = std::move(input.upload_id),
      .part_number = input.part_number,
  };
}

TransferSession ImportSession(
    const us3_turbo_access::gateway::OpenSessionResponse& response) {
  TransferSession session;
  session.meta.request_id = response.request_id();
  session.meta.session_id = response.session_id();
  session.meta.ticket = response.ticket();
  session.meta.expire_at = response.expire_at();
  session.meta.gateway_id = response.gateway_id().empty() ? kDefaultGatewayId : response.gateway_id();
  return session;
}

}  // namespace us3_turbo_access::client
