#include "client/src/core/rdma/rdma_multipart_flow.h"

#include "client/src/control/metadata_client.h"
#include "client/src/core/rdma/rdma_transfer_path.h"

namespace us3_turbo_access::client {

namespace {

class RdmaMultipartSession final : public IMultipartSession {
 public:
  RdmaMultipartSession(const MetadataClient& metadata,
                       const RdmaTransferPath& transfer_path,
                       ObjectId object,
                       std::string upload_id,
                       std::size_t max_part_size)
      : metadata_(metadata),
        transfer_path_(transfer_path),
        object_(std::move(object)),
        upload_id_(std::move(upload_id)),
        max_part_size_(max_part_size) {}

  const std::string& upload_id() const noexcept override { return upload_id_; }
  std::size_t        max_part_size() const noexcept override { return max_part_size_; }

  Result<TransferOutcome> UploadPart(std::uint32_t part_number,
                                     std::uint64_t object_offset,
                                     const std::string& checksum_policy,
                                     ConstBufferView buffer) override {
    RequestOptions request;
    request.object          = object_;
    request.offset          = object_offset;
    request.checksum_policy = checksum_policy;
    request.length          = buffer.size;  // server BindSession needs to pre-allocate
    return transfer_path_.PutObjectPart(request, buffer, upload_id_, part_number);
  }

  Result<CompleteResult> Complete(const std::vector<PartRef>& parts) override {
    std::vector<PartCompletion> rpc_parts;
    rpc_parts.reserve(parts.size());
    for (const auto& p : parts) rpc_parts.push_back({p.part_number, p.etag});
    auto out = metadata_.RpcCompleteMultipartUpload(
        upload_id_, rpc_parts, DataPath::kNativeRdma);
    if (!out.success()) return Result<CompleteResult>::Failure(out.error());
    return Result<CompleteResult>::Success(
        {out.value().etag, out.value().version, out.value().content_length});
  }

  Result<bool> Abort() override {
    return metadata_.RpcAbortMultipartUpload(upload_id_, DataPath::kNativeRdma);
  }

 private:
  const MetadataClient&   metadata_;
  const RdmaTransferPath& transfer_path_;
  ObjectId                object_;
  std::string             upload_id_;
  std::size_t             max_part_size_;
};

}  // namespace

RdmaMultipartFlow::RdmaMultipartFlow(const MetadataClient& metadata,
                                     const RdmaTransferPath& transfer_path)
    : metadata_(metadata), transfer_path_(transfer_path) {}

Result<std::unique_ptr<IMultipartSession>>
RdmaMultipartFlow::Start(const ObjectDescriptor& desc) {
  auto out = metadata_.RpcCreateMultipartUpload(desc);
  if (!out.success()) {
    return Result<std::unique_ptr<IMultipartSession>>::Failure(out.error());
  }
  auto session = std::make_unique<RdmaMultipartSession>(
      metadata_, transfer_path_, desc.object,
      std::move(out.value().upload_id), out.value().max_part_size);
  return Result<std::unique_ptr<IMultipartSession>>::Success(std::move(session));
}

}  // namespace us3_turbo_access::client
