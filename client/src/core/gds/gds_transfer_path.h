#pragma once

#include <cstdint>
#include <string>

#include "client/src/core/gds/gds_context.h"
#include "client/src/core/routing/transfer_path.h"

namespace us3_turbo_access::client {

class GdsTransferPath final : public TransferPath {
 public:
  GdsTransferPath(const PlatformCapabilities& caps, const GdsContext& context);

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
  const PlatformCapabilities& caps_;
  GdsContext context_;
};

}  // namespace us3_turbo_access::client
