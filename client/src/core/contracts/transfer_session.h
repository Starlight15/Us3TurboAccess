#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace us3_turbo_access::client {

struct SessionMeta {
  std::string request_id;
  std::string session_id;
  std::string ticket;
  std::string expire_at;
  std::string gateway_id;
  std::string gateway_endpoint;
  std::string channel_id;
  std::string async_handle;
};

struct ChunkPlanItem {
  std::uint64_t offset{0};
  std::uint64_t size{0};
};

struct TransferSession {
  SessionMeta meta;
  std::vector<ChunkPlanItem> chunk_plan;
  std::unordered_map<std::string, std::string> headers;
};

}  // namespace us3_turbo_access::client
