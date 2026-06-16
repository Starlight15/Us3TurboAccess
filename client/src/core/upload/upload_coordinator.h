#pragma once

#include <memory>

#include "client/src/core/upload/i_multipart_flow.h"
#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

class ClientCore;

// UploadCoordinator —— multipart flow 注册表。
//
// 当前职责只剩一件事：按 DataPath 把请求路由到对应的 IMultipartFlow
// （HTTP / RDMA / GDS）。三条 flow 在 Client 生命周期内常驻，调用方
// 拿到的引用 non-owning。
//
// 历史背景：早期版本曾在这里维护“跨链路通用 multipart 上传流程”，
// 现在那部分已下沉到各链路的 IMultipartFlow / IMultipartSession，
// 本类不再承担任何 multipart 协议实现，仅做路由分发。
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
