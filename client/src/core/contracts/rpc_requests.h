#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "client/src/core/contracts/rpc_context.h"
#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

/**
 * @brief Object addressing + transfer options shared between RPC payloads.
 */
struct ObjectRequest {
  ObjectId object;
  std::uint64_t offset{0};
  std::optional<std::uint64_t> length;
  DataPath data_path{DataPath::kGdsCuObject};
  BufferType buffer_type{BufferType::kHostRegular};
  std::string checksum_policy{"none"};
  std::unordered_map<std::string, std::string> extra_headers;
};

/**
 * @brief Request payload for MetadataClient::RpcOpenTransferSession.
 */
struct SessionOpening {
  RpcCallMetadata context;
  OperationType operation{OperationType::kGet};
  ObjectId object;
  DataPath data_path{DataPath::kGdsCuObject};
  BufferType buffer_type{BufferType::kHostRegular};
  std::uint64_t offset{0};
  std::optional<std::uint64_t> length;
  std::string request_id;
  std::string session_id;
  std::string idempotency_key;
  bool is_multipart_part{false};
};

/**
 * @brief Request payload for GdsDataClient::GdsChunk.
 */
struct ChunkOp {
  RpcCallMetadata context;
  ObjectRequest object;
  OperationType operation{OperationType::kGet};
  std::string request_id;
  std::string session_id;
  std::string transfer_ticket;
  std::string rdma_token;
  std::uint64_t chunk_offset{0};
  std::size_t chunk_size{0};
  std::string upload_id;       // empty for non-multipart
  std::uint32_t part_number{0};
};

}  // namespace us3_turbo_access::client
