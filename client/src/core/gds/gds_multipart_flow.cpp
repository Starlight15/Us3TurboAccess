#include "client/src/core/gds/gds_multipart_flow.h"

#include "client/src/control/metadata_client.h"
#include "client/src/core/gds/gds_transfer_path.h"

namespace us3_turbo_access::client {

GdsMultipartFlow::GdsMultipartFlow(const MetadataClient& metadata,
                                   const GdsTransferPath& transfer_path)
    : metadata_(metadata), transfer_path_(transfer_path) {}

Result<StartUploadResult> GdsMultipartFlow::StartMultipart(const ObjectDescriptor& desc) {
  return metadata_.RpcCreateMultipartUpload(desc);
}

Result<TransferOutcome> GdsMultipartFlow::UploadPart(
    const ObjectDescriptor& desc, const std::string& upload_id,
    std::uint32_t part_number, ConstBufferView buffer) {
  return transfer_path_.PutObjectPart(desc, buffer, upload_id, part_number);
}

Result<IMultipartFlow::CompleteResult> GdsMultipartFlow::CompleteMultipart(
    const std::string& upload_id, const std::vector<PartRef>& parts) {
  std::vector<PartCompletion> rpc_parts;
  rpc_parts.reserve(parts.size());
  for (const auto& p : parts) rpc_parts.push_back({p.part_number, p.etag});
  auto out = metadata_.RpcCompleteMultipartUpload(upload_id, rpc_parts, DataPath::kGdsCuObject);
  if (!out.success()) return Result<CompleteResult>::Failure(out.error());
  return Result<CompleteResult>::Success(
      {out.value().etag, out.value().version, out.value().content_length});
}

Result<bool> GdsMultipartFlow::AbortMultipart(const std::string& upload_id) {
  return metadata_.RpcAbortMultipartUpload(upload_id, DataPath::kGdsCuObject);
}

}  // namespace us3_turbo_access::client
