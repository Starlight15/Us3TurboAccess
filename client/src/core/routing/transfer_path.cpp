#include "client/src/core/routing/transfer_path.h"

#include <string>

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

namespace {

[[nodiscard]] std::string_view BufferTypeName(BufferType t) {
  switch (t) {
    case BufferType::kHostRegular: return "kHostRegular";
    case BufferType::kHostPinned:  return "kHostPinned";
    case BufferType::kCudaDevice:  return "kCudaDevice";
    default:                       return "<unknown>";
  }
}

}  // namespace

Result<bool> TransferPath::CommonPreflight(DataFlow path_for_error,
                                            std::string_view path_name,
                                            bool available, BufferType actual,
                                            BufferType required) {
  if (!available) {
    return Result<bool>::Failure(MakeUnsupportedPath(
        path_for_error,
        std::string(path_name) + " path is not available on this client"));
  }
  if (actual != required) {
    return Result<bool>::Failure(MakeUnsupportedPath(
        path_for_error,
        std::string(path_name) + " path requires buffer.type=" +
            std::string(BufferTypeName(required)) +
            ", got buffer.type=" + std::string(BufferTypeName(actual))));
  }
  return Result<bool>::Success(true);
}

}  // namespace us3_turbo_access::client
