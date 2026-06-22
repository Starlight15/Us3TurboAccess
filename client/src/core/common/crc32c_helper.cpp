#include "client/src/core/common/crc32c_helper.h"

#include <span>

#include <butil/crc32c.h>
#include <butil/base64.h>

namespace us3_turbo_access::client {

std::optional<std::uint32_t> ComputeClientCrc32c(ConstBufferView buffer,
                                                  bool send_crc32c) {
  if (!send_crc32c) return std::nullopt;
  if (buffer.data == nullptr || buffer.size == 0) return std::nullopt;

  return butil::crc32c::Value(static_cast<const char*>(buffer.data), buffer.size);
}

std::string Base64Crc32cBigEndian(std::uint32_t crc32c) {
  const unsigned char bytes[4] = {
      static_cast<unsigned char>((crc32c >> 24) & 0xFFU),
      static_cast<unsigned char>((crc32c >> 16) & 0xFFU),
      static_cast<unsigned char>((crc32c >> 8) & 0xFFU),
      static_cast<unsigned char>(crc32c & 0xFFU),
  };
  std::string out;
  butil::Base64Encode(butil::StringPiece(reinterpret_cast<const char*>(bytes), 4), &out);
  return out;
}

}  // namespace us3_turbo_access::client
