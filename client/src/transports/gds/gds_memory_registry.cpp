#include "client/src/transports/gds/gds_memory_registry.h"

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {
namespace {

template <typename Pointer>
[[nodiscard]] Result<RegisteredBuffer> BuildDescriptor(OperationType /*operation*/, Pointer data,
                                                       std::size_t size, BufferType type) {
  if (type != BufferType::kCudaDevice) {
    return Result<RegisteredBuffer>::Failure(
        MakeUnsupportedPath(DataPath::kGdsCuObject,
                            "The GDS channel only supports CUDA device buffers"));
  }
  if (data == nullptr || size == 0U) {
    return Result<RegisteredBuffer>::Failure(
        MakeInvalidArgument("GDS buffers must be non-null and have a positive size"));
  }
  return Result<RegisteredBuffer>::Success(RegisteredBuffer{});
}

}  // namespace

Result<RegisteredBuffer> GdsMemoryRegistry::Register(OperationType operation,
                                                     MutableBufferView buffer) const {
  return BuildDescriptor(operation, buffer.data, buffer.size, buffer.type);
}

Result<RegisteredBuffer> GdsMemoryRegistry::Register(OperationType operation,
                                                     ConstBufferView buffer) const {
  return BuildDescriptor(operation, buffer.data, buffer.size, buffer.type);
}

}  // namespace us3_turbo_access::client
