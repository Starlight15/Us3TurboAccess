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

// C.3：按 op 选择超时。head/put/get_timeout 为 0 时 fallback 到 request_timeout。
[[nodiscard]] std::chrono::milliseconds PerOpTimeout(const ClientOptions& options,
                                                       OperationType op) {
  switch (op) {
    case OperationType::kHead:
      return options.head_timeout.count() > 0 ? options.head_timeout
                                              : options.request_timeout;
    case OperationType::kPut:
      return options.put_timeout.count() > 0 ? options.put_timeout
                                             : options.request_timeout;
    case OperationType::kGet:
      return options.get_timeout.count() > 0 ? options.get_timeout
                                             : options.request_timeout;
  }
  return options.request_timeout;
}

[[nodiscard]] RpcCallMetadata MakeRpcContext(const ClientOptions& options,
                                                std::chrono::milliseconds timeout,
                                                OperationType op) {
  // 优先级：RequestOptions::timeout > per-op timeout > request_timeout
  const auto effective =
      (timeout.count() <= 0) ? PerOpTimeout(options, op) : timeout;
  return RpcCallMetadata{.client_id = options.client_id,
                           .bearer_token = options.bearer_token,
                           .default_headers = options.default_headers,
                           .timeout = effective};
}

}  // namespace

SessionOpening MakeSessionHandshake(const ClientOptions& options,
                                            const SessionPlan& input) {
  return SessionOpening{
      .context = MakeRpcContext(options, input.timeout, input.operation),
      .operation = input.operation,
      .object = input.object,
      .data_flow = input.path,
      .buffer_type = input.buffer_type,
      .offset = input.offset,
      .length = input.length,
      .request_id = MakeId(kRequestIdPrefix),
      .session_id = MakeId(kSessionIdPrefix),
      .idempotency_key = input.idempotency_key,
      .is_multipart_part = input.is_multipart_part,
  };
}

ChunkOp MakeChunkOp(const ClientOptions& options, ChunkOpPlan input) {
  ObjectRequest object{
      .object = input.object,
      .offset = input.offset,
      .length = input.length,
      .data_flow = input.path,
      .buffer_type = input.buffer_type,
      .checksum_policy = input.checksum_policy,
      .extra_headers = input.extra_headers,
  };
  return ChunkOp{
      .context = MakeRpcContext(options, input.timeout, input.operation),
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
