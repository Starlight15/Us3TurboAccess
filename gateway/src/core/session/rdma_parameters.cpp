#include "core/session/rdma_parameters.h"

namespace us3_turbo_access::gateway::core {

std::unordered_map<std::string, std::string> RdmaParameters::ToMap() const {
  std::unordered_map<std::string, std::string> map = extras;
  if (!host.empty()) {
    map["host"] = host;
  }
  if (!port.empty()) {
    map["port"] = port;
  }
  return map;
}

RdmaParameters RdmaParameters::FromMap(
    const std::unordered_map<std::string, std::string>& map) {
  RdmaParameters out;
  for (const auto& [k, v] : map) {
    if (k == "host") {
      out.host = v;
    } else if (k == "port") {
      out.port = v;
    } else {
      out.extras[k] = v;
    }
  }
  return out;
}

}  // namespace us3_turbo_access::gateway::core
