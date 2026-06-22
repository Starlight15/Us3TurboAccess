#pragma once

#include <memory>

#include "client/src/core/upload/i_multipart_flow.h"
#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

class ClientCore;

// UploadCoordinator —— multipart flow 注册表。
//
// 当前职责：按 DataFlow 把请求路由到对应的 IMultipartFlow（GDS / RDMA）。
class UploadCoordinator {
 public:
  explicit UploadCoordinator(ClientCore& core);

  [[nodiscard]] IMultipartFlow& SelectFlow(DataFlow data_flow);

 private:
  std::unique_ptr<IMultipartFlow> rdma_flow_;
  std::unique_ptr<IMultipartFlow> gds_flow_;
};

}  // namespace us3_turbo_access::client
