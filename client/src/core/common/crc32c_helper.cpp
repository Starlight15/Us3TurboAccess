#include "client/src/core/common/crc32c_helper.h"

#include <span>

#include "client/src/data/http_crc32c.h"

namespace us3_turbo_access::client {

std::optional<std::uint32_t> ComputeClientCrc32c(ConstBufferView buffer,
                                                  bool send_crc32c) {
  if (!send_crc32c) return std::nullopt;
  if (buffer.data == nullptr || buffer.size == 0) return std::nullopt;

  const auto bytes = std::span<const std::byte>(
      static_cast<const std::byte*>(buffer.data), buffer.size);
  return Crc32c(bytes);
}

}  // namespace us3_turbo_access::client
