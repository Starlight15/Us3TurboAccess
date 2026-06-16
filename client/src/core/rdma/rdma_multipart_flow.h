#pragma once

#include "client/src/core/upload/i_multipart_flow.h"

namespace us3_turbo_access::client {

class MetadataClient;
class RdmaTransferPath;

class RdmaMultipartFlow final : public IMultipartFlow {
 public:
  RdmaMultipartFlow(const MetadataClient& metadata, const RdmaTransferPath& transfer_path);

  Result<StartUploadResult> StartMultipart(const ObjectDescriptor& desc) override;
  Result<TransferOutcome> UploadPart(const ObjectDescriptor& desc,
                                     const std::string& upload_id,
                                     std::uint32_t part_number,
                                     ConstBufferView buffer) override;
  Result<CompleteResult> CompleteMultipart(const std::string& upload_id,
                                           const std::vector<PartRef>& parts) override;
  Result<bool> AbortMultipart(const std::string& upload_id) override;

 private:
  const MetadataClient&   metadata_;
  const RdmaTransferPath& transfer_path_;
};

}  // namespace us3_turbo_access::client
