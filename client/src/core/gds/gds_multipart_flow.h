#pragma once

#include "client/src/core/upload/i_multipart_flow.h"

namespace us3_turbo_access::client {

class MetadataClient;
class GdsTransferPath;

// GdsMultipartFlow / GdsMultipartSession 走 GDS（cuObject / cuFile）路径：
//   RpcCreateMultipartUpload (control plane baidu_std)
//   → 每 part：OpenSession + AcquireToken + GdsChunk（gateway 主动从
//              client GPU buffer 拉数据） + CommitPart
//   → RpcCompleteMultipartUpload / RpcAbortMultipartUpload
// length 在 part 级别故意不设，gateway 跳过 whole-object Reserve。
class GdsMultipartFlow final : public IMultipartFlow {
 public:
  GdsMultipartFlow(const MetadataClient& metadata, const GdsTransferPath& transfer_path);

  Result<std::unique_ptr<IMultipartSession>>
    CreateSession(const ObjectDescriptor& desc) override;

 private:
  const MetadataClient& metadata_;
  const GdsTransferPath& transfer_path_;
};

}  // namespace us3_turbo_access::client
