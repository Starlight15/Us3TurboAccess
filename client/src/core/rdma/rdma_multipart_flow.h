#pragma once

#include "client/src/core/upload/i_multipart_flow.h"

namespace us3_turbo_access::client {

class MetadataClient;
class RdmaTransferPath;

// RdmaMultipartFlow / RdmaMultipartSession 走 UCX RMA 路径的 multipart：
//   RpcCreateMultipartUpload (control plane baidu_std)
//   → 每 part：PrepareTransfer → ucp_put_nbx + AM_WRITE_DONE → CommitPart
//   → RpcCompleteMultipartUpload / RpcAbortMultipartUpload
// data plane 由 RdmaTransferPath 拥有的 UcxWorker / EndpointPool 驱动。
class RdmaMultipartFlow final : public IMultipartFlow {
 public:
  RdmaMultipartFlow(const MetadataClient& metadata, const RdmaTransferPath& transfer_path);

  Result<std::unique_ptr<IMultipartSession>>
    CreateSession(const ObjectDescriptor& desc) override;

 private:
  const MetadataClient&   metadata_;
  const RdmaTransferPath& transfer_path_;
};

}  // namespace us3_turbo_access::client
