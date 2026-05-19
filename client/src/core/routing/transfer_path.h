#pragma once

#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

class TransferPath {
 public:
  virtual ~TransferPath() = default;

  [[nodiscard]] virtual DataPath path() const = 0;
  [[nodiscard]] virtual bool available() const = 0;
  [[nodiscard]] virtual Result<TransferOutcome> GetObject(const RequestOptions& request,
                                                          MutableBufferView buffer) const = 0;
  [[nodiscard]] virtual Result<TransferOutcome> PutObject(const RequestOptions& request,
                                                          ConstBufferView buffer) const = 0;
};

}  // namespace us3_turbo_access::client
