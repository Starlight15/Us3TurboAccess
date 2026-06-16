#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "client/src/core/gds/gds_context.h"
#include "client/src/core/routing/transfer_path.h"

namespace us3_turbo_access::client {

class ClientExecutor;

class GdsTransferPath final : public TransferPath {
 public:
  /**
   * executor_provider 给 GetObject 并发分片用（与 HttpTransferPath 对称）：
   * 参数 `() -> ClientExecutor*`，让 GdsTransferPath 不直接持线程池引用
   * （executor 在 ClientCore 延后初始化）。返回 nullptr 时降级单连接。
   */
  using ExecutorProvider = std::function<ClientExecutor*()>;

  GdsTransferPath(const PlatformCapabilities& caps, const GdsContext& context,
                  ExecutorProvider executor_provider = nullptr);

  [[nodiscard]] DataPath path() const override;
  [[nodiscard]] bool available() const override;
  [[nodiscard]] Result<TransferOutcome> GetObject(const RequestOptions& request,
                                                  MutableBufferView buffer) const override;
  [[nodiscard]] Result<TransferOutcome> PutObject(const RequestOptions& request,
                                                  ConstBufferView buffer) const override;

  /**
   * Multipart 单 part 上传：与 RdmaTransferPath / HttpTransferPath 的
   * PutObjectPart 对称。
   *
   * 走 control plane baidu_std（OpenSession + ExecutePutPart）；
   * expected_size 透传 0 保留 GDS 原来的 Reserve 跳过行为。
   * 返回的 TransferOutcome.etag 是 part_etag。
   */
  [[nodiscard]] Result<TransferOutcome>
    PutObjectPart(const RequestOptions& request, ConstBufferView buffer,
                  const std::string& upload_id,
                  std::uint32_t part_number) const;

 private:
  // 单连接降级：length 小 / 并发不够 / executor 未就绪 时走这条路径。
  [[nodiscard]] Result<TransferOutcome>
    GetObjectSingle(const RequestOptions& request,
                    MutableBufferView buffer) const;
  // 并发分片：把 [offset, offset+length) 切 N 块，各自独立 OpenSession + Get。
  [[nodiscard]] Result<TransferOutcome>
    GetObjectParallel(const RequestOptions& request,
                      MutableBufferView buffer,
                      ClientExecutor& executor) const;

  const PlatformCapabilities& caps_;
  GdsContext context_;
  ExecutorProvider executor_provider_;
};

}  // namespace us3_turbo_access::client
