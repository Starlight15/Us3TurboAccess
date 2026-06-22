#pragma once

#include <cstdint>
#include <string>

#include "client/src/core/contracts/rpc_requests.h"
#include "client/src/core/contracts/transfer_session.h"
#include "control_plane.pb.h"
#include "us3_turbo_access/client/options.h"

namespace us3_turbo_access::client {

// 装配 SessionOpening 所需的输入参数集合。
struct SessionPlan {
  OperationType operation;
  ObjectId object;
  std::uint64_t offset{0};
  std::optional<std::uint64_t> length;
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
  std::string idempotency_key;
  BufferType buffer_type;
  DataFlow path;
  bool is_multipart_part{false};
};

// 装配 ChunkOp 所需的输入参数集合。
struct ChunkOpPlan {
  OperationType operation;
  ObjectId object;
  std::uint64_t offset{0};
  std::optional<std::uint64_t> length;
  std::string checksum_policy{"none"};
  std::unordered_map<std::string, std::string> extra_headers;
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
  BufferType buffer_type;
  DataFlow path;
  std::string request_id;
  std::string session_id;
  std::string ticket;
  std::string rdma_token;
  std::uint64_t chunk_offset{0};
  std::size_t chunk_size{0};
  std::string upload_id;
  std::uint32_t part_number{0};
};

[[nodiscard]] SessionOpening MakeSessionHandshake(const ClientOptions& options,
                                                          const SessionPlan& input);
[[nodiscard]] ChunkOp MakeChunkOp(const ClientOptions& options,
                                                     ChunkOpPlan input);
[[nodiscard]] TransferSession ImportSession(
    const us3_turbo_access::gateway::OpenSessionResponse& response);

}  // namespace us3_turbo_access::client
