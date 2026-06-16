#include "client/src/core/gds/gds_multipart_flow.h"

#include "client/src/control/metadata_client.h"
#include "client/src/core/common/errors.h"
#include "client/src/core/gds/gds_transfer_path.h"

namespace us3_turbo_access::client {

namespace {

class GdsMultipartSession final : public IMultipartSession {
 public:
  GdsMultipartSession(const MetadataClient& metadata,
                      const GdsTransferPath& transfer_path,
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
    // length intentionally unset: gateway skips whole-object Reserve on session open
    return transfer_path_.PutObjectPart(request, buffer, upload_id_, part_number);
  }

  Result<CompleteResult> Complete(const std::vector<PartRef>& parts) override {
    std::vector<PartCompletion> rpc_parts;
    rpc_parts.reserve(parts.size());
    for (const auto& p : parts) rpc_parts.push_back({p.part_number, p.etag});
    auto out = metadata_.RpcCompleteMultipartUpload(
        upload_id_, rpc_parts, DataPath::kGdsCuObject);
    if (!out.success()) return Result<CompleteResult>::Failure(out.error());
    return Result<CompleteResult>::Success(
        {out.value().etag, out.value().version, out.value().content_length});
  }

  Result<bool> Abort() override {
    return metadata_.RpcAbortMultipartUpload(upload_id_, DataPath::kGdsCuObject);
  }

 private:
  const MetadataClient&  metadata_;
  const GdsTransferPath& transfer_path_;
  ObjectId               object_;
  std::string            upload_id_;
  std::size_t            max_part_size_;
};

}  // namespace

GdsMultipartFlow::GdsMultipartFlow(const MetadataClient& metadata,
                                   const GdsTransferPath& transfer_path)
    : metadata_(metadata), transfer_path_(transfer_path) {}

Status GdsMultipartFlow::CreateSession(const ObjectDescriptor& desc,
                                       std::unique_ptr<IMultipartSession>* out) {
  if (out == nullptr) {
    return Status::FromError(MakeInvalidArgument("CreateSession: out is null"));
  }
  auto r = metadata_.RpcCreateMultipartUpload(desc);
  if (!r.success()) return Status::FromError(r.error());
  *out = std::make_unique<GdsMultipartSession>(
      metadata_, transfer_path_, desc.object,
      std::move(r.value().upload_id), r.value().max_part_size);
  return Status::Ok();
}

}  // namespace us3_turbo_access::client
