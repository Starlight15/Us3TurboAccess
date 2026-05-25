#pragma once

#include <atomic>
#include <cstdint>

#include "backend/index_store.h"

namespace us3_turbo_access::gateway::backend {

/**
 * 空索引：所有方法成功，Head 返回零字节占位 meta，StartMultipart 返回单调递增 id。
 * 用于 wire 链路验证，不存任何持久状态。
 */
class NullIndexStore final : public IIndexStore {
 public:
  NullIndexStore() = default;

  [[nodiscard]] std::string_view kind() const noexcept override { return "null-index"; }

  [[nodiscard]] Result<ObjectMetadata>
    Head(std::string_view bucket, std::string_view key) override;

  [[nodiscard]] Result<bool>
    PutObjectIndex(std::string_view bucket, std::string_view key,
                   std::size_t content_length, std::string_view etag,
                   std::string_view version) override;

  [[nodiscard]] Result<bool>
    DeleteObjectIndex(std::string_view bucket, std::string_view key) override;

  [[nodiscard]] Result<std::string>
    StartMultipart(std::string_view bucket, std::string_view key,
                   std::optional<std::size_t> total_size_hint) override;

  [[nodiscard]] Result<bool>
    AbortMultipart(std::string_view upload_id) override;

  [[nodiscard]] Result<ObjectMetadata>
    CommitMultipart(std::string_view upload_id, std::string_view bucket,
                    std::string_view key, std::size_t content_length,
                    std::string_view etag, std::string_view version) override;

 private:
  std::atomic<std::uint64_t> seq_{0};
};

}  // namespace us3_turbo_access::gateway::backend
