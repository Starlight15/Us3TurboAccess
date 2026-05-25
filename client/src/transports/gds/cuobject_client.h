#pragma once

#include <cstdint>
#include <string>

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

  /**
   * @brief PUT a single multipart part. The cuObject transfer attributes all
   *        chunks to (upload_id, part_number) on the gateway.
   */
  [[nodiscard]] Result<TransferOutcome> ExecutePutPart(
      const ClientOptions& options, const GdsDataClient& data_client,
      const TransferSession& session, const RequestOptions& request,
      ConstBufferView buffer, const std::string& upload_id,
      std::uint32_t part_number) const;
};

}  // namespace us3_turbo_access::client
