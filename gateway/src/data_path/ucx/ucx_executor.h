#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/logger.h>
#include <ucp/api/ucp.h>

#include "data_path/ucx/ucx_session_registry.h"
#include "data_path/ucx/ucx_buffer_pool.h"
#include "data_path/data_path_executor.h"
#include "us3_turbo_access/gateway/options.h"
#include "us3_turbo_access/gateway/result.h"

namespace us3_turbo_access::gateway::backend {
class IBackend;
}

namespace us3_turbo_access::gateway::core {
class MetadataService;
}

namespace us3_turbo_access::gateway::data_path::ucx {

/** PrepareTransfer 返回给 client。 */
struct UcxDiscoverInfo {
  std::string              host;
  std::uint32_t            ucx_port{0};
  std::uint64_t            max_msg_bytes{0};
  std::uint64_t            gw_raddr{0};        // gateway buffer 虚地址
  std::vector<std::uint8_t> gw_packed_rkey;    // ucp_rkey_pack 结果
};

struct UcxCommitInfo    { std::string etag; std::string version; };
struct UcxCommitPartInfo{ std::string part_etag; };

/**
 * Gateway 侧 UCX 数据面（RMA WRITE 模式）。
 *
 * 流程（4 步）：
 *   OpenSession → PrepareTransfer(→ gw_raddr/rkey) → client ucp_put_nbx+flush+AM_DONE
 *   → CommitObject(等 write_done → CRC → WriteRange)
 *
 * UCX worker 为 MULTI 模式；progress_thread_ 驱动 NIC 完成回调。
 *
 * Multipart part 的业务编排（Lookup / RegisterPart）已迁至
 * UcxMultipartPathHandler；本类只保留纯数据写入能力。
 */
class UcxExecutor final : public data_path::IDataPathExecutor {
 public:
  // AM handler ID：client 写完后发此 AM 携带 session_id，触发 write_done
  static constexpr std::uint8_t kAmIdWriteDone = 0;

  UcxExecutor(std::string public_host, std::string bind_host,
              const UcxOptions& opts,
              backend::IBackend& backend,
              core::MetadataService& metadata,
              std::shared_ptr<spdlog::logger> logger);
  ~UcxExecutor() override;

  UcxExecutor(const UcxExecutor&) = delete;
  UcxExecutor& operator=(const UcxExecutor&) = delete;

  [[nodiscard]] DataPath    kind()      const noexcept override { return DataPath::kNativeRdma; }
  [[nodiscard]] bool        available() const override {
    return started_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::string endpoint()  const override {
    return public_host_ + ":" + std::to_string(opts_.listen_port);
  }
  [[nodiscard]] Result<bool> Start()  override;
  void                       Stop()   override;
  [[nodiscard]] Result<bool> OnSessionOpened(const core::Session& session) override;

  [[nodiscard]] int ucx_port() const noexcept { return opts_.listen_port; }

  /**
   * Gateway 分配+注册 gw_buf，返回 gw_raddr/gw_packed_rkey 供 client ucp_put_nbx 使用。
   */
  [[nodiscard]] Result<UcxDiscoverInfo>
    PrepareTransfer(std::string_view session_id,
                    std::uint64_t transfer_bytes);

  /**
   * 异步 CommitObject：write_done 已置位时同步完成并返回结果；
   * 否则把落盘逻辑存入 entry->pending_commit，由 OnAmWriteDone 触发，
   * 通过 on_done 回调把结果传出（在 ProgressLoop 线程调用）。
   * 返回 true = 同步完成（on_done 不会被调用），false = 异步等待。
   */
  [[nodiscard]] bool
    CommitObjectAsync(std::string_view session_id,
                      std::uint64_t bytes_transferred,
                      std::string_view client_crc32c_b64,
                      std::function<void(Result<UcxCommitInfo>)> on_done);

  /**
   * @brief Async commit of a multipart part's data to the backend.
   *
   * This is a low-level data-path operation with NO multipart orchestration
   * (no Lookup, no RegisterPart). The caller (UcxMultipartPathHandler) is
   * responsible for the upload lifecycle and part registration.
   *
   * @param session_id         UCX session ID
   * @param backend_upload_id  Backend-internal upload ID (resolved by caller)
   * @param part_number        1-based part number
   * @param bytes_transferred  Number of bytes the client wrote
   * @param client_crc32c_b64  Optional base64-encoded CRC32C from client
   * @param on_done            Completion callback; receives the backend etag.
   * @return true = sync (on_done called), false = async pending.
   */
  [[nodiscard]] bool
    CommitPartDataAsync(std::string_view session_id,
                        std::string_view backend_upload_id,
                        std::uint32_t part_number,
                        std::uint64_t bytes_transferred,
                        std::string_view client_crc32c_b64,
                        std::function<void(Result<std::string>)> on_done);

  [[nodiscard]] Result<bool> AbortSession(std::string_view session_id);

 private:
  static void OnConnRequest(ucp_conn_request_h conn_req, void* arg);

  static ucs_status_t OnAmWriteDone(void* arg,
                                     const void* header, std::size_t header_length,
                                     void* data, std::size_t data_length,
                                     const ucp_am_recv_param_t* param);

  void ProgressLoop();
  void ReleaseEntry(std::shared_ptr<UcxSessionEntry> entry);

  // CommitObject 落盘逻辑，由同步路径或 pending_commit 调用
  [[nodiscard]] Result<UcxCommitInfo>
    DoCommitObject(std::shared_ptr<UcxSessionEntry>& entry,
                   std::uint64_t bytes_transferred,
                   std::string_view client_crc32c_b64);

  // CommitPartData 低层落盘逻辑：CRC + backend WritePart，无 multipart 语义
  [[nodiscard]] Result<std::string>
    DoWritePartData(std::shared_ptr<UcxSessionEntry>& entry,
                    std::string_view backend_upload_id, std::uint32_t part_number,
                    std::uint64_t bytes_transferred,
                    std::string_view client_crc32c_b64);

  std::string                                    public_host_;
  std::string                                    bind_host_;
  UcxOptions                                     opts_;
  backend::IBackend&                             backend_;
  core::MetadataService&                         metadata_;
  std::shared_ptr<spdlog::logger>                logger_;

  ucp_context_h                                  ucp_ctx_{nullptr};
  ucp_worker_h                                   ucp_worker_{nullptr};
  ucp_listener_h                                 ucp_listener_{nullptr};

  // accepted ep 列表：OnConnRequest（progress thread）写，Stop（app thread）在 join 后读
  std::vector<ucp_ep_h>                          accepted_eps_;

  std::atomic<bool>                              stop_{false};
  std::atomic<bool>                              started_{false};
  std::thread                                    progress_thread_;

  std::unique_ptr<UcxSessionRegistry>            session_registry_;
  std::unique_ptr<UcxBufferPool>                 buffer_pool_;
};

}  // namespace us3_turbo_access::gateway::data_path::ucx
