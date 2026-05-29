#include "backend/backend.h"

#include <butil/iobuf.h>

namespace us3_turbo_access::gateway::backend {

// 默认实现：回退到 span 版本（需要 to_string() 拷贝）
Result<ObjectMetadata> IBackend::Write(std::string_view bucket,
                                       std::string_view key,
                                       const butil::IOBuf& src) {
  const auto payload = src.to_string();
  std::span<const std::byte> span(
      reinterpret_cast<const std::byte*>(payload.data()), payload.size());
  return Write(bucket, key, span);
}

// 默认实现：回退到 span 版本（需要 to_string() 拷贝）
Result<std::string> IBackend::WritePart(std::string_view upload_id,
                                        std::uint32_t part_number,
                                        std::uint64_t offset,
                                        const butil::IOBuf& src) {
  const auto payload = src.to_string();
  std::span<const std::byte> span(
      reinterpret_cast<const std::byte*>(payload.data()), payload.size());
  return WritePart(upload_id, part_number, offset, span);
}

}  // namespace us3_turbo_access::gateway::backend
