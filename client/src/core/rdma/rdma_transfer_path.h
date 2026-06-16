#pragma once

#include <atomic>
#include <memory>

#include <ucp/api/ucp.h>

#include "client/src/control/metadata_client.h"
#include "client/src/core/routing/transfer_path.h"
#include "client/src/data/rdma_data_plane_client.h"
#include "client/src/transports/ucx/ucx_context.h"
#include "client/src/transports/ucx/ucx_endpoint_pool.h"
#include "client/src/transports/ucx/ucx_worker.h"
#include "us3_turbo_access/client/options.h"

namespace us3_turbo_access::client {

/**
 * UCX RMA WRITE 传输路径。
 *
 * 每次 PutObject/PutObjectPart 流程（长连接池复用）：
 *   OpenSession → PrepareTransfer RPC（→ gw_raddr/gw_rkey）
 *   → pool.Acquire(ep+rkey) → ucp_put_nbx + AM_WRITE_DONE
 *   → CommitObject/CommitPart RPC → pool.Return(ep)
 *
 * gateway 等收到 AM_WRITE_DONE 后直接从自己的 buffer 落盘，无需 GET。
 * UcxWorker 内置 ProgressLoop 后台线程驱动 NIC 完成回调，调用方无需 spin。
 *
 * 已知限制（v1）：
 *   单 ucx_worker_ 实例，多线程并发 PutObjectPart 时所有 ucp_put_nbx 共享
 *   同一 worker 内部锁，并发 > 8 线程后吞吐不再线性增长。
 *   如需更高并发，需实现 Worker Pool（每线程独立 worker）。
 */
class RdmaTransferPath final : public TransferPath {
 public:
  RdmaTransferPath(const ClientOptions& options,
                   const MetadataClient& metadata_client,
                   const RdmaDataPlaneClient& data_plane_client);
  ~RdmaTransferPath() override;

  void SetAvailable(bool available) noexcept;

  [[nodiscard]] DataPath path() const override;
  [[nodiscard]] bool     available() const override;

  [[nodiscard]] Result<TransferOutcome>
    GetObject(const RequestOptions& request, MutableBufferView buffer) const override;

  [[nodiscard]] Result<TransferOutcome>
    PutObject(const RequestOptions& request, ConstBufferView buffer) const override;

  [[nodiscard]] Result<TransferOutcome>
    PutObjectPart(const RequestOptions& request, ConstBufferView buffer,
                  const std::string& upload_id, std::uint32_t part_number) const;

 private:
  // PrepareAndWrite 返回值：PutAndNotify 完成时 gw_rkey 已在 slot 里；ep 成功时归还 pool
  struct WritePrepared {
    std::string session_id;
    std::string request_id;
    std::string gateway_id;
    std::string ep_host;
    uint16_t    ep_port{0};
    EpSlot      slot{};    // ep + rkey，成功归还 pool，失败 Discard

    WritePrepared() = default;

    // RAII 支持
    const RdmaTransferPath* owner   = nullptr;
    bool                    released = false;

    ~WritePrepared() {
      if (!released && owner && slot.ep) {
        owner->ReturnEp(*this, false);  // 析构时默认失败归还（Discard）
      }
    }

    // 禁止拷贝，允许移动
    WritePrepared(const WritePrepared&) = delete;
    WritePrepared& operator=(const WritePrepared&) = delete;
    WritePrepared(WritePrepared&& other) noexcept
        : session_id(std::move(other.session_id)),
          request_id(std::move(other.request_id)),
          gateway_id(std::move(other.gateway_id)),
          ep_host(std::move(other.ep_host)),
          ep_port(other.ep_port),
          slot(other.slot),
          owner(other.owner),
          released(other.released) {
      other.released = true;  // 移动后源对象不再管理
    }
    WritePrepared& operator=(WritePrepared&&) = delete;

    // 显式 release：成功时归还 pool，失败时 Discard
    void release(bool success) {
      if (owner && slot.ep) {
        owner->ReturnEp(*this, success);
        released = true;
      }
    }
  };

  [[nodiscard]] Result<WritePrepared>
    PrepareAndWrite(const RequestOptions& request,
                    ConstBufferView buffer,
                    bool is_multipart_part,
                    std::chrono::steady_clock::time_point deadline) const;

  // CommitObject 后归还 slot 到 pool（成功）或 Discard（失败）
  void ReturnEp(WritePrepared& prepared, bool success) const noexcept;

  const ClientOptions&       options_;
  const MetadataClient&      metadata_client_;
  const RdmaDataPlaneClient& data_plane_client_;

  std::unique_ptr<UcxContext>      ucx_ctx_;
  std::unique_ptr<UcxWorker>       ucx_worker_;
  std::unique_ptr<UcxEndpointPool> ucx_pool_;
  std::atomic<bool>                available_{false};
};

}  // namespace us3_turbo_access::client
