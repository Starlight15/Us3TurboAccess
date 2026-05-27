#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

std::string_view ToString(DataPath path) {
  switch (path) {
    case DataPath::kGdsCuObject:
      return "gds-cuobject";
    case DataPath::kNativeRdma:
      return "native-rdma";
    case DataPath::kHttpTcp:
      return "http-tcp";
  }
  return "unknown";
}

std::string_view ToString(BufferType type) {
  switch (type) {
    case BufferType::kHostRegular:
      return "host-regular";
    case BufferType::kHostPinned:
      return "host-pinned";
    case BufferType::kCudaDevice:
      return "cuda-device";
  }
  return "unknown";
}

std::string_view ToString(OperationType operation) {
  switch (operation) {
    case OperationType::kGet:
      return "GET";
    case OperationType::kPut:
      return "PUT";
    case OperationType::kHead:
      return "HEAD";
  }
  return "UNKNOWN";
}

}  // namespace us3_turbo_access::client
