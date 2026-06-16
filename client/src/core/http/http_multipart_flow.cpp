#include "client/src/core/http/http_multipart_flow.h"

#include "client/src/core/common/errors.h"
#include "client/src/core/http/http_transfer_path.h"
#include "client/src/data/http_data_client.h"

namespace us3_turbo_access::client {

namespace {

class HttpMultipartSession final : public IMultipartSession {
 public:
  HttpMultipartSession(HttpDataClient& data_client,
                       const HttpTransferPath& transfer_path,
                       ObjectId object,
                       std::string upload_id,
                       std::size_t max_part_size)
      : data_client_(data_client),
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
    request.length          = buffer.size;
    return transfer_path_.PutObjectPart(request, buffer, upload_id_, part_number);
  }

  Result<CompleteResult> Complete(const std::vector<PartRef>& parts) override {
    std::vector<HttpDataClient::PartEtag> http_parts;
    http_parts.reserve(parts.size());
    for (const auto& p : parts) http_parts.push_back({p.part_number, p.etag, std::nullopt});
    auto out = data_client_.CompleteUpload(upload_id_, http_parts);
    if (!out.success()) return Result<CompleteResult>::Failure(out.error());
    return Result<CompleteResult>::Success(
        {out.value().etag, out.value().version, out.value().content_length});
  }

  Result<bool> Abort() override { return data_client_.AbortUpload(upload_id_); }

 private:
  HttpDataClient&         data_client_;
  const HttpTransferPath& transfer_path_;
  ObjectId                object_;
  std::string             upload_id_;
  std::size_t             max_part_size_;
};

}  // namespace

HttpMultipartFlow::HttpMultipartFlow(HttpDataClient& data_client,
                                     const HttpTransferPath& transfer_path)
    : data_client_(data_client), transfer_path_(transfer_path) {}

Status HttpMultipartFlow::CreateSession(const ObjectDescriptor& desc,
                                        std::unique_ptr<IMultipartSession>* out) {
  if (out == nullptr) {
    return Status::FromError(MakeInvalidArgument("CreateSession: out is null"));
  }
  auto r = data_client_.StartUpload(
      desc.object,
      static_cast<std::uint64_t>(desc.expected_total_size.value_or(0)),
      desc.idempotency_key);
  if (!r.success()) return Status::FromError(r.error());
  *out = std::make_unique<HttpMultipartSession>(
      data_client_, transfer_path_, desc.object,
      std::move(r.value().upload_id), r.value().max_part_size);
  return Status::Ok();
}

}  // namespace us3_turbo_access::client
