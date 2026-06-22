#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <spdlog/logger.h>

#include "backend/backend.h"
#include "core/multipart/multipart.h"
#include "core/session/session.h"
#include "data_path/data_path_executor.h"
#include "us3_turbo_access/gateway/options.h"
#include "us3_turbo_access/gateway/result.h"

class cuObjServer;

namespace us3_turbo_access::gateway::data_flow::gds {
class PinnedBufferPool;
}  // namespace us3_turbo_access::gateway::data_flow::gds

namespace us3_turbo_access::gateway::core {
class MetadataService;
}  // namespace us3_turbo_access::gateway::core

namespace us3_turbo_access::gateway::data_flow::gds {

/**
 * @brief GDS / cuObject server-side data-path executor.
 *
 * Wraps `cuObjServer` and performs server-side RDMA chunk transfers. Owned by
 * the gateway runtime, configured by `GatewayOptions::gds_*`. Implements the
 * shared lifecycle contract via @ref IDataPathExecutor and additionally exposes
 * GDS-specific `GetChunk` / `PutChunk` calls used by the control plane.
 */
class GdsExecutor final : public IDataPathExecutor {
 public:
  GdsExecutor(std::string public_host, std::string bind_host,
              const GdsOptions& opts, backend::IBackend& backend,
              core::MetadataService& metadata,
              std::shared_ptr<spdlog::logger> logger);
  ~GdsExecutor() override;

  [[nodiscard]] DataFlow kind() const noexcept override {
    return DataFlow::GPUDirect;
  }
  [[nodiscard]] bool        available() const override;
  [[nodiscard]] std::string endpoint() const override;
  [[nodiscard]] Result<bool> Start() override;
  void                       Stop() override;

  /**
   * @brief Path-specific session open hook.
   *
   * Validates GDS availability and reserves backend space for PUT sessions
   * with a known expected_size.
   */
  [[nodiscard]] Result<bool>
    OnSessionOpened(const core::Session& session) override;

  [[nodiscard]] int                port() const noexcept { return opts_.rdma_port; }
  [[nodiscard]] const std::string& bind_host() const noexcept { return bind_host_; }

  /**
   * @brief GetChunk 返回值：RDMA reply 字符串 + server 端 CRC32C。
   *
   * CRC 在 pinned buffer 阶段计算（backend 读完 → RDMA 推送前），让 client
   * 做 end-to-end 校验。crc32c==0 表示 empty / 未计算。
   */
  struct GetChunkOutcome {
    std::string   rdma_reply;
    std::uint32_t crc32c{0};
  };

  /**
   * @brief Server-side RDMA WRITE: reads `length` bytes from backend at
   *        `object_offset` and pushes them to the client GPU.
   */
  [[nodiscard]] Result<GetChunkOutcome>
    GetChunk(const core::Session& session, const std::string& rdma_token,
             std::uint64_t object_offset, std::uint64_t length);

  /**
   * @brief Server-side RDMA READ: pulls `length` bytes from the client GPU
   *        into a host buffer and persists them into the backend at
   *        `object_offset`.
   */
  [[nodiscard]] Result<ObjectMetadata>
    PutChunk(const core::Session& session, const std::string& rdma_token,
             std::uint64_t object_offset, std::uint64_t length);

  /**
   * @brief Server-side RDMA READ into a multipart part.
   *
   * Pulls @p length bytes from the client GPU and stages them via
   * `backend.WritePart(upload_id, part_number, ...)`. Returns the part's
   * etag on success.
   *
   * @param upload  Optional MultipartUpload for cross-chunk MD5 accumulation
   *                (checksum_policy=="md5"). Pass nullptr for non-md5 paths.
   */
  [[nodiscard]] Result<std::string>
    PutPart(const core::Session& session, const std::string& rdma_token,
            const std::string& upload_id, std::uint32_t part_number,
            std::uint64_t object_offset, std::uint64_t length,
            std::string_view checksum_policy,
            core::multipart::MultipartUpload* upload = nullptr);

 private:
  [[nodiscard]] Result<std::shared_ptr<cuObjServer>> GetServer() const;

  std::string                       public_host_;
  std::string                       bind_host_;
  GdsOptions                        opts_;
  backend::IBackend&                backend_;
  core::MetadataService&            metadata_;
  std::shared_ptr<spdlog::logger>   logger_;
  mutable std::mutex                mu_;
  std::shared_ptr<cuObjServer>      server_;
  std::shared_ptr<PinnedBufferPool> buffer_pool_;
};

}  // namespace us3_turbo_access::gateway::data_flow::gds
