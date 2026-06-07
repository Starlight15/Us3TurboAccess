#pragma once

#include <cstdint>
#include <optional>

#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

/**
 * 统一计算客户端 CRC32C 校验和，供 HTTP / GDS / UCX 三条路径复用。
 *
 * @param buffer      源 buffer
 * @param send_crc32c 对应路径的 options.send_crc32c 开关
 * @return 若开关打开且 buffer 有效，返回 CRC32C 值；否则返回空 optional。
 */
std::optional<std::uint32_t> ComputeClientCrc32c(ConstBufferView buffer,
                                                  bool send_crc32c);

}  // namespace us3_turbo_access::client
