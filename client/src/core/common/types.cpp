#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

std::string_view ToString(DataFlow flow) {
  switch (flow) {
    case DataFlow::NONE:
      return "none";
    case DataFlow::GPUDirect:
      return "gpu-direct";
    case DataFlow::CPUDirect:
      return "cpu-direct";
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
