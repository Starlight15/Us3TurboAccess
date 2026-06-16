#include "client/src/core/rdma/rdma_multipart_flow.h"

#include "client/src/control/metadata_client.h"
#include "client/src/core/rdma/rdma_transfer_path.h"

namespace us3_turbo_access::client {

RdmaMultipartFlow::RdmaMultipartFlow(const MetadataClient& metadata,
                                     const RdmaTransferPath& transfer_path)
    : metadata_(metadata), transfer_path_(transfer_path) {}

Result<StartUploadResult> RdmaMultipartFlow::StartMultipart(const ObjectDescriptor& desc) {
  return metadata_.RpcCreateMultipartUpload(desc);
}

Result<TransferOutcome> RdmaMultipartFlow::UploadPart(
    const ObjectDescriptor& desc, const std::string& upload_id,
    std::uint32_t part_number, ConstBufferView buffer) {
  return transfer_path_.PutObjectPart(desc, buffer, upload_id, part_number);
}

Result<IMultipartFlow::CompleteResult> RdmaMultipartFlow::CompleteMultipart(
    const std::string& upload_id, const std::vector<PartRef>& parts) {
  std::vector<PartCompletion> rpc_parts;
  rpc_parts.reserve(parts.size());
  for (const auto& p : parts) rpc_parts.push_back({p.part_number, p.etag});
  auto out = metadata_.RpcCompleteMultipartUpload(upload_id, rpc_parts, DataPath::kNativeRdma);
  if (!out.success()) return Result<CompleteResult>::Failure(out.error());
  return Result<CompleteResult>::Success(
      {out.value().etag, out.value().version, out.value().content_length});
}

Result<bool> RdmaMultipartFlow::AbortMultipart(const std::string& upload_id) {
  return metadata_.RpcAbortMultipartUpload(upload_id, DataPath::kNativeRdma);
}

}  // namespace us3_turbo_access::client
