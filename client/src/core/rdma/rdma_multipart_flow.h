#pragma once

#include "client/src/core/upload/i_multipart_flow.h"

namespace us3_turbo_access::client {

class MetadataClient;
class RdmaTransferPath;

class RdmaMultipartFlow final : public IMultipartFlow {
 public:
  RdmaMultipartFlow(const MetadataClient& metadata, const RdmaTransferPath& transfer_path);

  Result<std::unique_ptr<IMultipartSession>>
    Start(const ObjectDescriptor& desc) override;

 private:
  const MetadataClient&   metadata_;
  const RdmaTransferPath& transfer_path_;
};

}  // namespace us3_turbo_access::client
