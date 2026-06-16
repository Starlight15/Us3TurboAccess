#pragma once

#include "client/src/core/upload/i_multipart_flow.h"

namespace us3_turbo_access::client {

class HttpDataClient;
class HttpTransferPath;

class HttpMultipartFlow final : public IMultipartFlow {
 public:
  HttpMultipartFlow(HttpDataClient& data_client, const HttpTransferPath& transfer_path);

  Result<std::unique_ptr<IMultipartSession>>
    Start(const ObjectDescriptor& desc) override;

 private:
  HttpDataClient&         data_client_;
  const HttpTransferPath& transfer_path_;
};

}  // namespace us3_turbo_access::client
