#pragma once

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

 private:
  const PlatformCapabilities& caps_;
  GdsContext context_;
};

}  // namespace us3_turbo_access::client
