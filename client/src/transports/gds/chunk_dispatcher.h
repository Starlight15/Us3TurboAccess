#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "client/src/core/contracts/request_builder.h"
#include "client/src/core/contracts/transfer_session.h"
#include "client/src/data/gds_data_client.h"
#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

/**
 * @brief 在 cuObject 数据面回调中向 gateway 发送 chunk RPC 的派发器。
 *
 * 构造时持有发送 chunk RPC 所需的全部稳定上下文（GdsDataClient、ClientOptions、
 * 会话与请求快照）。callback 内仅需注入本次 chunk 的 rdma_token / offset / size。
 */
class ChunkDispatcher {
 public:
  ChunkDispatcher(const ClientOptions& options, const GdsDataClient& data_client,
                  const TransferSession& session, const RequestOptions& request, OperationType op);

  struct Outcome {
    std::string gateway_id;
    std::string transfer_status;
    std::string rdma_reply;
    std::string etag;
    std::string version;
  };

  /**
   * @brief 发送一次 chunk RPC。
   *
   * @param rdma_token 来自 cuObject 回调的 RDMA token；为空时直接报错。
   * @param chunk_offset 本次 chunk 在对象内的绝对偏移。
   * @param chunk_size 本次 chunk 的字节数。
   * @return 成功时返回响应中的状态字段；失败时返回错误。
   */
  [[nodiscard]] Result<Outcome> Dispatch(const std::string& rdma_token,
                                          std::uint64_t chunk_offset,
                                          std::size_t chunk_size) const;

 private:
  const GdsDataClient& data_client_;
  const ClientOptions& options_;
  OperationType op_;
  RequestOptions request_;
  std::string request_id_;
  std::string session_id_;
  std::string ticket_;
};

}  // namespace us3_turbo_access::client
