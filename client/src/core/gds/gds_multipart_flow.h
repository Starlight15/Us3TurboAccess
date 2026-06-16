#pragma once

#include "client/src/core/upload/i_multipart_flow.h"

namespace us3_turbo_access::client {

class MetadataClient;
class GdsTransferPath;

class GdsMultipartFlow final : public IMultipartFlow {
 public:
  GdsMultipartFlow(const MetadataClient& metadata, const GdsTransferPath& transfer_path);

  Result<std::unique_ptr<IMultipartSession>>
    Start(const ObjectDescriptor& desc) override;

 private:
  const MetadataClient& metadata_;
  const GdsTransferPath& transfer_path_;
};

}  // namespace us3_turbo_access::client
