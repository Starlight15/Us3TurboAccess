#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <spdlog/logger.h>

#include "backend/backend.h"
#include "core/session/session.h"
#include "data_path/data_path_executor.h"
#include "us3_turbo_access/gateway/options.h"
#include "us3_turbo_access/gateway/result.h"
#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway::core {
class MetadataService;
}  // namespace us3_turbo_access::gateway::core

namespace us3_turbo_access::gateway::core::multipart {
class MultipartCoordinator;
}  // namespace us3_turbo_access::gateway::core::multipart

namespace us3_turbo_access::gateway::runtime {
class IoWorkerPool;
}  // namespace us3_turbo_access::gateway::runtime

namespace us3_turbo_access::gateway::data_path::rdma {

class RdmaResources;
class RdmaListener;
class RdmaSessionRegistry;
class RdmaConnectionRegistry;
class RdmaSessionSweeper;

/** DiscoverRdmaEndpoint 的服务端结果（只含 endpoint 信息）。 */
struct RdmaDiscoverInfo {
  std::string   host;
  std::uint32_t port{0};
  std::uint64_t max_msg_bytes{0};
};

/** BindSessionToConnection 的服务端结果（session 级 buffer 凭据）。 */
struct RdmaBindInfo {
  std::uint64_t raddr{0};
  std::uint32_t rkey{0};
};

/** CommitObject / CommitPart 的服务端结果。 */
struct RdmaCommitInfo {
  std::string etag;
  std::string version;
};
struct RdmaCommitPartInfo {
  std::string part_etag;
};

/**
 * Native RDMA 数据面 executor。
 *
 * 生命周期：
 *   Start                   → 探设备 + 起 listener
 *   OnSessionOpened         → 在 SessionRegistry 登记 session（仅做 backend reserve）
 *   client CM connect       → listener 在 cm_id->verbs 上建 PD/CQ/QP；
 *                             分配 conn_token 注册到 ConnectionRegistry，
 *                             ACCEPT 时通过 private_data 回传 conn_token
 *   BindSessionToConnection → 用 conn_token 找 PD，分配 buffer + reg MR，挂到 session entry
 *   CommitObject            → backend.WriteRange + 释放 session 级 buffer（连接保留）
 *   client CM disconnect    → DISCONNECTED 事件时 ConnectionRegistry 回收连接级资源
 */
class RdmaExecutor final : public IDataPathExecutor {
 public:
  RdmaExecutor(std::string public_host, std::string bind_host,
               const RdmaOptions& opts, backend::IBackend& backend,
               core::MetadataService& metadata,
               core::multipart::MultipartCoordinator* multipart,
               runtime::IoWorkerPool* io_pool,
               std::shared_ptr<spdlog::logger> logger);
  ~RdmaExecutor() override;

  [[nodiscard]] DataPath kind() const noexcept override {
    return DataPath::kNativeRdma;
  }
  [[nodiscard]] bool        available() const override;
  [[nodiscard]] std::string endpoint() const override;
  [[nodiscard]] Result<bool> Start() override;
  void                       Stop() override;

  [[nodiscard]] Result<bool>
    OnSessionOpened(const core::Session& session) override;

  /** DiscoverRdmaEndpoint RPC 入口：只返回 gateway 端点信息。*/
  [[nodiscard]] Result<RdmaDiscoverInfo>
    DiscoverEndpoint(std::string_view session_id);

  /** BindSessionToConnection RPC 入口：在指定 connection 的 PD 上为 session 注册 buffer/MR。*/
  [[nodiscard]] Result<RdmaBindInfo>
    BindSessionToConnection(std::string_view session_id, std::uint64_t conn_token);

  /** CommitObject RPC 入口：触发单对象 PUT 落盘 + 释放 session 级 buffer。*/
  [[nodiscard]] Result<RdmaCommitInfo>
    CommitObject(std::string_view session_id, std::uint64_t bytes_transferred);

  /** CommitPart RPC 入口：触发 multipart part 落盘。*/
  [[nodiscard]] Result<RdmaCommitPartInfo>
    CommitPart(std::string_view session_id, std::string_view upload_id,
               std::uint32_t part_number, std::uint64_t bytes_transferred);

  /** AbortSession RPC 入口：客户端失败路径调用，释放该 session 的 buffer/MR。
   *  返回是否实际擦除了一条 entry（不存在视为 no-op，仍返回 success）。*/
  [[nodiscard]] Result<bool> AbortSession(std::string_view session_id);

  [[nodiscard]] const RdmaResources* resources() const noexcept {
    return resources_.get();
  }
  [[nodiscard]] RdmaSessionRegistry* session_registry() const noexcept {
    return session_registry_.get();
  }
  [[nodiscard]] RdmaConnectionRegistry* connection_registry() const noexcept {
    return connection_registry_.get();
  }

 private:
  std::string                                public_host_;
  std::string                                bind_host_;
  RdmaOptions                                opts_;
  backend::IBackend&                         backend_;
  core::MetadataService&                     metadata_;
  core::multipart::MultipartCoordinator*     multipart_{nullptr};
  runtime::IoWorkerPool*                     io_pool_{nullptr};
  std::shared_ptr<spdlog::logger>            logger_;

  mutable std::mutex                         mu_;
  std::shared_ptr<RdmaResources>             resources_;
  std::unique_ptr<RdmaSessionRegistry>       session_registry_;
  std::unique_ptr<RdmaConnectionRegistry>    connection_registry_;
  std::unique_ptr<RdmaListener>              listener_;
  std::unique_ptr<RdmaSessionSweeper>        sweeper_;
  bool                                       started_{false};
};

}  // namespace us3_turbo_access::gateway::data_path::rdma
