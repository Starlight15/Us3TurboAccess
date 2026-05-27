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
 * @brief GDS 数据面 chunk RPC 发起器（GdsChunk RPC 客户端薄包装）。
 *
 * 构造时绑定一次会话上下文（GdsDataClient、ClientOptions、TransferSession +
 * RequestOptions 快照），Dispatch 内只接 (rdma_token, chunk_offset, chunk_size)
 * 三个变化参数。token-direct 数据面 (cuobject_client.cpp::ExecuteTransfer) 每
 * 个 PUT/GET 调一次 Dispatch；multipart 模式下 SetMultipart 把绝对 offset 转
 * part 内偏移。
 */
class ChunkDispatcher {
 public:
  ChunkDispatcher(const ClientOptions& options, const GdsDataClient& data_client,
                  const TransferSession& session, const RequestOptions& request, OperationType op);

  void SetMultipart(std::string upload_id, std::uint32_t part_number);

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
  std::string upload_id_;
  std::uint32_t part_number_{0};
  std::uint64_t part_base_offset_{0};
};

}  // namespace us3_turbo_access::client
