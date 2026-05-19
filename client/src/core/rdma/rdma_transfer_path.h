#pragma once

#include "client/src/core/routing/transfer_path.h"

namespace us3_turbo_access::client {

class RdmaTransferPath final : public TransferPath {
 public:
  RdmaTransferPath() = default;

  [[nodiscard]] DataPath path() const override;
  [[nodiscard]] bool available() const override;
  [[nodiscard]] Result<TransferOutcome> GetObject(const RequestOptions& request,
                                                  MutableBufferView buffer) const override;
  [[nodiscard]] Result<TransferOutcome> PutObject(const RequestOptions& request,
                                                  ConstBufferView buffer) const override;
};

}  // namespace us3_turbo_access::client
