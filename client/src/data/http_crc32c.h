#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace us3_turbo_access::client {

/**
 * CRC-32C (Castagnoli) 软件实现，与 gateway 同名实现物理隔离。
 *
 * 故意不共用一份代码：client 是 SDK 给上层用，gateway 是 daemon 进程，
 * 各自独立编译/部署；任何对算法的改动必须两端同步，通过 http_verify_example
 * 双向校验确认 wire 行为一致。对端实现见 gateway/src/common/crc32c.{h,cpp}。
 *
 * Base64Crc32cBigEndian：把 32-bit CRC 按 S3 约定（big-endian 4 字节 + base64）
 * 编码进 x-amz-checksum-crc32c 头。
 *
 * DecodeBase64Crc32cBigEndian：反向；从 server 响应头 x-amz-checksum-crc32c
 * 取回 CRC32C。格式不合规返回 nullopt。
 */

[[nodiscard]] std::uint32_t Crc32c(std::span<const std::byte> data) noexcept;
[[nodiscard]] std::string   Base64Crc32cBigEndian(std::uint32_t crc);
[[nodiscard]] std::optional<std::uint32_t>
                            DecodeBase64Crc32cBigEndian(std::string_view s);

}  // namespace us3_turbo_access::client
