#include "client/src/data/http_crc32c.h"

#include <array>

namespace us3_turbo_access::client {

namespace {

// Castagnoli reflected polynomial（== bit-reverse(0x1EDC6F41)）。
constexpr std::uint32_t kPoly = 0x82F63B78u;

constexpr std::array<std::uint32_t, 256> MakeTable() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1U) ? ((c >> 1) ^ kPoly) : (c >> 1);
    }
    t[i] = c;
  }
  return t;
}

constexpr auto kTable = MakeTable();

constexpr char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::uint32_t Crc32c(std::span<const std::byte> data) noexcept {
  std::uint32_t state = 0xFFFFFFFFu;
  for (std::byte b : data) {
    state = (state >> 8) ^
            kTable[(state ^ static_cast<std::uint8_t>(b)) & 0xFFu];
  }
  return state ^ 0xFFFFFFFFu;
}

std::string Base64Crc32cBigEndian(std::uint32_t crc) {
  // 4 字节 big-endian → 6 字符 base64 (3字节->4字符，4字节填到 6字节 padding 一个 '=')
  const std::uint8_t b[4] = {
      static_cast<std::uint8_t>((crc >> 24) & 0xFF),
      static_cast<std::uint8_t>((crc >> 16) & 0xFF),
      static_cast<std::uint8_t>((crc >> 8) & 0xFF),
      static_cast<std::uint8_t>(crc & 0xFF),
  };
  // 第一组：b[0..2] → 4 chars。
  // 第二组：b[3] 占用 8 bit，编码出 2 char + "=="；S3 用 "==" 补齐 8 字符。
  std::string out;
  out.reserve(8);
  out.push_back(kBase64[b[0] >> 2]);
  out.push_back(kBase64[((b[0] & 0x03) << 4) | (b[1] >> 4)]);
  out.push_back(kBase64[((b[1] & 0x0F) << 2) | (b[2] >> 6)]);
  out.push_back(kBase64[b[2] & 0x3F]);
  out.push_back(kBase64[b[3] >> 2]);
  out.push_back(kBase64[(b[3] & 0x03) << 4]);
  out.push_back('=');
  out.push_back('=');
  return out;
}

namespace {
// 严格按标准 alphabet + '=' padding 解 base64，只接受 4 字节明文（即 S3
// CRC32C 的固定长度）。不合规的输入返回 nullopt 让调用方走 fallback。
constexpr int IndexOfBase64(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
}  // namespace

std::optional<std::uint32_t> DecodeBase64Crc32cBigEndian(std::string_view s) {
  std::uint8_t bytes[4]{};
  std::size_t  produced = 0;
  std::uint32_t buf = 0;
  int           bits = 0;
  for (char c : s) {
    if (c == '=' || c == '\r' || c == '\n' || c == ' ') {
      if (c == '=') break;  // padding ends content
      continue;             // 忽略空白
    }
    const int v = IndexOfBase64(c);
    if (v < 0) return std::nullopt;
    buf = (buf << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (produced >= sizeof(bytes)) return std::nullopt;
      bytes[produced++] = static_cast<std::uint8_t>((buf >> bits) & 0xFFu);
    }
  }
  if (produced != 4) return std::nullopt;
  return (static_cast<std::uint32_t>(bytes[0]) << 24) |
         (static_cast<std::uint32_t>(bytes[1]) << 16) |
         (static_cast<std::uint32_t>(bytes[2]) << 8) |
         static_cast<std::uint32_t>(bytes[3]);
}

}  // namespace us3_turbo_access::client
