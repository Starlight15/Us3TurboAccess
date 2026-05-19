#pragma once

#include "client/src/core/contracts/transfer_session.h"
#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

class GdsDataClient;

class CuObjectClient {
 public:
  CuObjectClient() = default;

  [[nodiscard]] Result<TransferOutcome> ExecuteGet(const ClientOptions& options,
                                                   const GdsDataClient& data_client,
                                                   const TransferSession& session,
                                                   const RequestOptions& request,
                                                   MutableBufferView buffer) const;

  [[nodiscard]] Result<TransferOutcome> ExecutePut(const ClientOptions& options,
                                                   const GdsDataClient& data_client,
                                                   const TransferSession& session,
                                                   const RequestOptions& request,
                                                   ConstBufferView buffer) const;
};

}  // namespace us3_turbo_access::client
