#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "us3_turbo_access/gateway/result.h"
#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway::backend {

/**
 * @brief 已写入的某个 part 的描述（part_number + 后端返回的 etag）。
 */
struct PartRecord {
  std::uint32_t part_number{0};
  std::string   etag;
};

/**
 * @brief 比较器：按 part_number 升序。给 std::sort / std::map 等用。
 */
inline bool PartRecordByNumberLess(const PartRecord& a, const PartRecord& b) {
  return a.part_number < b.part_number;
}

/**
 * @brief Abstract object storage that the gateway delegates to.
 *
 * The first version ships in-memory stubs (`MemoryBackend`, `NullBackend`).
 * The interface is intentionally narrow so the real backend can be plugged in
 * later without disturbing the rest of the gateway.
 *
 * Multipart 接口（StartMultipart / WritePart / CompleteMultipart /
 * AbortMultipart）把对象的索引提交与字节写入分成两阶段；非 multipart 的简单
 * 路径仍走 Write / WriteRange。
 */
class IBackend {
 public:
  virtual ~IBackend() = default;

  [[nodiscard]] virtual std::string_view kind() const noexcept = 0;

  /**
   * @brief Returns object metadata, or kNotFound when the key is unknown.
   */
  [[nodiscard]] virtual Result<ObjectMetadata>
    Head(std::string_view bucket, std::string_view key) = 0;

  /**
   * @brief Reads up to `dst.size()` bytes starting at @p offset.
   *
   * @return number of bytes copied into @p dst (may be 0 when the offset is
   *         past the end of the object).
   */
  [[nodiscard]] virtual Result<std::size_t>
    Read(std::string_view bucket, std::string_view key,
         std::uint64_t offset, std::span<std::byte> dst) = 0;

  /**
   * @brief Stores @p src as the full object content (offset = 0).
   *
   * Equivalent to `WriteRange(bucket, key, 0, src, src.size())`.
   */
  [[nodiscard]] virtual Result<ObjectMetadata>
    Write(std::string_view bucket, std::string_view key,
          std::span<const std::byte> src) = 0;

  /**
   * @brief Writes @p src at @p offset, optionally pre-sizing to @p total_size.
   *
   * Used by chunked uploads (GDS / future RDMA). When @p total_size is empty
   * the object grows to fit; when provided, the body is pre-allocated to that
   * size on first write so subsequent chunks can land in place.
   */
  [[nodiscard]] virtual Result<ObjectMetadata>
    WriteRange(std::string_view bucket, std::string_view key,
               std::uint64_t offset, std::span<const std::byte> src,
               std::optional<std::size_t> total_size) = 0;

  /**
   * @brief Reserves @p total_size bytes for @p bucket/@p key without writing.
   *
   * Used by the control plane to pre-allocate the object on session open.
   * Each backend must provide its own semantics (memory: resize buffer;
   * object-storage: start multipart / pre-allocate); there is no portable
   * default.
   */
  [[nodiscard]] virtual Result<ObjectMetadata>
    Reserve(std::string_view bucket, std::string_view key,
            std::size_t total_size) = 0;

  /**
   * @brief 创建一个 multipart 上传，返回 upload_id（索引层面）。
   */
  [[nodiscard]] virtual Result<std::string>
    StartMultipart(std::string_view bucket, std::string_view key,
                   std::optional<std::size_t> total_size_hint) = 0;

  /**
   * @brief 写入一个 part 的字节，返回 part 的 etag。
   */
  [[nodiscard]] virtual Result<std::string>
    WritePart(std::string_view upload_id, std::uint32_t part_number,
              std::uint64_t offset, std::span<const std::byte> src) = 0;

  /**
   * @brief 提交所有 part，写最终对象索引。
   */
  [[nodiscard]] virtual Result<ObjectMetadata>
    CompleteMultipart(std::string_view upload_id,
                      const std::vector<PartRecord>& parts) = 0;

  /**
   * @brief 放弃 multipart：删索引、释放已写部分。
   */
  [[nodiscard]] virtual Result<bool>
    AbortMultipart(std::string_view upload_id) = 0;
};

}  // namespace us3_turbo_access::gateway::backend
