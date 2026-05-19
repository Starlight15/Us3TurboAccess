#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway {

std::string_view ToString(OperationType op) noexcept {
  switch (op) {
    case OperationType::kGet:  return "GET";
    case OperationType::kPut:  return "PUT";
    case OperationType::kHead: return "HEAD";
  }
  return "UNKNOWN";
}

std::string_view ToString(DataPath path) noexcept {
  switch (path) {
    case DataPath::kHttpTcp:     return "http-tcp";
    case DataPath::kNativeRdma:  return "native-rdma";
    case DataPath::kGdsCuObject: return "gds-cuobject";
  }
  return "unknown";
}

DataPath ParseDataPath(std::string_view text) noexcept {
  if (text == "gds-cuobject") return DataPath::kGdsCuObject;
  if (text == "native-rdma")  return DataPath::kNativeRdma;
  return DataPath::kHttpTcp;
}

OperationType ParseOperationType(std::string_view text) noexcept {
  if (text == "PUT")  return OperationType::kPut;
  if (text == "HEAD") return OperationType::kHead;
  return OperationType::kGet;
}

}  // namespace us3_turbo_access::gateway
