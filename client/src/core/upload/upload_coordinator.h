#pragma once

#include <memory>

#include "client/src/core/upload/i_multipart_flow.h"
#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

class ClientCore;

class UploadCoordinator {
 public:
  explicit UploadCoordinator(ClientCore& core);

  // Returns the flow for the given data_path (non-owning, valid for Client lifetime).
  [[nodiscard]] IMultipartFlow& SelectFlow(DataPath data_path);

 private:
  std::unique_ptr<IMultipartFlow> http_flow_;
  std::unique_ptr<IMultipartFlow> rdma_flow_;
  std::unique_ptr<IMultipartFlow> gds_flow_;
};

}  // namespace us3_turbo_access::client
