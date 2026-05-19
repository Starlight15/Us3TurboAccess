#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <spdlog/logger.h>

#include "backend/backend.h"
#include "core/session/session.h"
#include "data_path/data_path_executor.h"
#include "us3_turbo_access/gateway/result.h"

class cuObjServer;

namespace us3_turbo_access::gateway::data_path::gds {

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
  GdsExecutor(std::string public_host, std::string bind_host, int port,
              backend::IBackend& backend,
              std::shared_ptr<spdlog::logger> logger);
  ~GdsExecutor() override;

  [[nodiscard]] DataPath kind() const noexcept override {
    return DataPath::kGdsCuObject;
  }
  [[nodiscard]] bool        available() const override;
  [[nodiscard]] std::string endpoint() const override;
  [[nodiscard]] Result<bool> Start() override;
  void                       Stop() override;

  [[nodiscard]] int                port() const noexcept { return port_; }
  [[nodiscard]] const std::string& bind_host() const noexcept { return bind_host_; }

  /**
   * @brief Server-side RDMA WRITE: reads `length` bytes from backend at
   *        `object_offset` and pushes them to the client GPU.
   */
  [[nodiscard]] Result<std::string>
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

 private:
  [[nodiscard]] Result<std::shared_ptr<cuObjServer>> GetServer() const;

  std::string                       public_host_;
  std::string                       bind_host_;
  int                               port_{0};
  backend::IBackend&                backend_;
  std::shared_ptr<spdlog::logger>   logger_;
  mutable std::mutex                mu_;
  std::shared_ptr<cuObjServer>      server_;
};

}  // namespace us3_turbo_access::gateway::data_path::gds
