#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

std::optional<std::uint32_t> ComputeClientCrc32c(ConstBufferView buffer,
                                                  bool send_crc32c);

/** CRC32C 值转 big-endian base64（S3 兼容的 x-amz-checksum-crc32c 格式）。 */
std::string Base64Crc32cBigEndian(std::uint32_t crc32c);

}  // namespace us3_turbo_access::client
