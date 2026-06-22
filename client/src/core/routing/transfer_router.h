#pragma once

#include "client/src/core/routing/transfer_path.h"

namespace us3_turbo_access::client {

/**
 * @brief 根据 ClientOptions::data_flow 选择 GDS / RDMA 路径执行器。
 *
 * 路径选择由配置决定；当对应路径不可用时直接报错，不做静默回退。
 */
class TransferRouter {
 public:
  TransferRouter(DataFlow data_flow, const TransferPath& gds_executor,
                 const TransferPath& rdma_executor);

  [[nodiscard]] Result<TransferOutcome> GetObject(const GetObjectRequest& request,
                                                  MutableBufferView buffer) const;
  [[nodiscard]] Result<TransferOutcome> PutObject(const PutObjectRequest& request,
                                                  ConstBufferView buffer) const;

 private:
  [[nodiscard]] const TransferPath* SelectTransferPath() const;

  DataFlow data_path_;
  const TransferPath& gds_executor_;
  const TransferPath& rdma_executor_;
};

}  // namespace us3_turbo_access::client
