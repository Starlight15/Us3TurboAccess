#pragma once

#include <atomic>
#include <memory>

#include "client/src/control/metadata_client.h"
#include "client/src/core/routing/transfer_path.h"
#include "client/src/data/rdma_data_plane_client.h"
#include "us3_turbo_access/client/options.h"

namespace us3_turbo_access::client {

class RdmaCompletionDriver;
class RdmaConnectionPool;

/**
 * Native RDMA 传输路径（CPU host buffer ↔ gateway pinned buffer）。
 *
 * 持有 RdmaCompletionDriver + RdmaConnectionPool；每次 PutObject 从池子借连接、
 * 在该连接的 PD 上注册 MR、提交 WRITE，完成事件经 driver 派发回 worker。
 * 路径不可用时（available()==false）driver/pool 仍存在但不会被触发。
 */
class RdmaTransferPath final : public TransferPath {
 public:
  RdmaTransferPath(const ClientOptions& options,
                   const MetadataClient& metadata_client,
                   const RdmaDataPlaneClient& data_plane_client);
  ~RdmaTransferPath() override;

  /** ClientCore::Initialize 成功探到 RDMA 设备后调 true；Shutdown 时调 false。*/
  void SetAvailable(bool available) noexcept;

  [[nodiscard]] DataPath path() const override;
  [[nodiscard]] bool available() const override;
  [[nodiscard]] Result<TransferOutcome> GetObject(const RequestOptions& request,
                                                   MutableBufferView buffer) const override;
  [[nodiscard]] Result<TransferOutcome> PutObject(const RequestOptions& request,
                                                   ConstBufferView buffer) const override;

  /**
   * Multipart upload 的单 part RDMA 写：与 PutObject 同流程，区别在于
   *   - OpenSession 带 is_multipart_part=true，gateway 跳过整对象 Reserve
   *   - 落盘走 CommitPart 而非 CommitObject，返回 part_etag（写到 outcome.etag）
   */
  [[nodiscard]] Result<TransferOutcome>
    PutObjectPart(const RequestOptions& request, ConstBufferView buffer,
                  const std::string& upload_id, std::uint32_t part_number) const;

 private:
  const ClientOptions&                  options_;
  const MetadataClient&                 metadata_client_;
  const RdmaDataPlaneClient&            data_plane_client_;
  std::unique_ptr<RdmaCompletionDriver> driver_;
  std::unique_ptr<RdmaConnectionPool>   pool_;
  std::atomic<bool>                     available_{false};
};

}  // namespace us3_turbo_access::client
