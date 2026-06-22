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

std::string_view ToString(DataFlow flow) noexcept {
  switch (flow) {
    case DataFlow::NONE:       return "none";
    case DataFlow::CPUDirect:  return "cpu-direct";
    case DataFlow::GPUDirect:  return "gpu-direct";
  }
  return "unknown";
}

DataFlow ParseDataFlow(std::string_view text) noexcept {
  if (text == "gpu-direct") return DataFlow::GPUDirect;
  if (text == "cpu-direct") return DataFlow::CPUDirect;
  if (text == "none")       return DataFlow::NONE;
  return DataFlow::NONE;
}

OperationType ParseOperationType(std::string_view text) noexcept {
  if (text == "PUT")  return OperationType::kPut;
  if (text == "HEAD") return OperationType::kHead;
  return OperationType::kGet;
}

}  // namespace us3_turbo_access::gateway
