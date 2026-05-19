#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

#include <spdlog/logger.h>

#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

/**
 * @brief Configures a client instance and its default request behavior.
 */
struct ClientOptions {
  std::string endpoint;
  std::string client_id{"us3-turbo-access-client"};
  std::string bearer_token;
  std::unordered_map<std::string, std::string> default_headers;
  std::chrono::milliseconds default_timeout{std::chrono::milliseconds(30000)};
  DataPath data_path{DataPath::kGdsCuObject};
  std::size_t default_chunk_size{8U * 1024U * 1024U};
  std::shared_ptr<spdlog::logger> logger;
};

}  // namespace us3_turbo_access::client
