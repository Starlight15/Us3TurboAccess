#pragma once

#include "client/src/core/upload/i_multipart_flow.h"

namespace us3_turbo_access::client {

class HttpDataClient;
class HttpTransferPath;

// HttpMultipartFlow / HttpMultipartSession 实现标准 S3 风格的 multipart：
//   StartUpload (HTTP)
//   → PutObjectPart (HTTP，每 part 一次 PUT，gateway 返回 part_etag)
//   → CompleteUpload / AbortUpload (HTTP)
// 整条链路只走 HttpDataClient + HttpTransferPath，不依赖 control plane RPC。
class HttpMultipartFlow final : public IMultipartFlow {
 public:
  HttpMultipartFlow(HttpDataClient& data_client, const HttpTransferPath& transfer_path);

  Status CreateSession(const ObjectDescriptor& desc,
                       std::unique_ptr<IMultipartSession>* out) override;

 private:
  HttpDataClient&         data_client_;
  const HttpTransferPath& transfer_path_;
};

}  // namespace us3_turbo_access::client
