#include "client/src/core/rdma/rdma_multipart_flow.h"

#include "client/src/control/metadata_client.h"
#include "client/src/core/common/errors.h"
#include "client/src/core/rdma/rdma_transfer_path.h"
#include "client/src/core/upload/multipart_session_base.h"

namespace us3_turbo_access::client {

namespace {

class RdmaMultipartSession final : public MultipartSessionBase {
 public:
  RdmaMultipartSession(const MetadataClient& metadata,
                       const RdmaTransferPath& transfer_path,
                       ObjectId object,
                       std::string upload_id,
                       std::size_t max_part_size)
      : MultipartSessionBase(std::move(object), std::move(upload_id),
                             max_part_size, /*set_length=*/true),
        metadata_(metadata),
        transfer_path_(transfer_path) {}

  // ---- 传输层转发 ----
  // set_length=true: server BindSession needs to pre-allocate buffer.
  Result<TransferOutcome> DoPutObjectPart(
      const RequestOptions& request, ConstBufferView buffer,
      const std::string& upload_id, std::uint32_t part_number) override {
    return transfer_path_.PutObjectPart(request, buffer, upload_id, part_number);
  }

  // ---- 协议层：UCX 走 MetadataClient RPC ----
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
};

}  // namespace

RdmaMultipartFlow::RdmaMultipartFlow(const MetadataClient& metadata,
                                     const RdmaTransferPath& transfer_path)
    : metadata_(metadata), transfer_path_(transfer_path) {}

Status RdmaMultipartFlow::CreateSession(const ObjectDescriptor& desc,
                                        std::unique_ptr<IMultipartSession>* out) {
  if (out == nullptr) {
    return Status::FromError(MakeInvalidArgument("CreateSession: out is null"));
  }
  auto r = metadata_.RpcCreateMultipartUpload(desc);
  if (!r.success()) return Status::FromError(r.error());
  *out = std::make_unique<RdmaMultipartSession>(
      metadata_, transfer_path_, desc.object,
      std::move(r.value().upload_id), r.value().max_part_size);
  return Status::Ok();
}

}  // namespace us3_turbo_access::client
