#pragma once

#include <string>

namespace us3_turbo_access::client {

struct SessionMeta {
  std::string request_id;
  std::string session_id;
  std::string ticket;
  std::string expire_at;
  std::string gateway_id;
};

struct TransferSession {
  SessionMeta meta;
};

}  // namespace us3_turbo_access::client
