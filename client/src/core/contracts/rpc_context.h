#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

namespace us3_turbo_access::client {

/**
 * @brief Header / auth / timeout context attached to every RPC call.
 */
struct RpcCallMetadata {
  std::string client_id;
  std::string bearer_token;
  std::unordered_map<std::string, std::string> default_headers;
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
};

}  // namespace us3_turbo_access::client
