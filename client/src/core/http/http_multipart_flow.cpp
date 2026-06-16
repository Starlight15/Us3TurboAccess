#include "client/src/core/http/http_multipart_flow.h"

#include "client/src/core/http/http_transfer_path.h"
#include "client/src/data/http_data_client.h"

namespace us3_turbo_access::client {

HttpMultipartFlow::HttpMultipartFlow(HttpDataClient& data_client,
                                     const HttpTransferPath& transfer_path)
    : data_client_(data_client), transfer_path_(transfer_path) {}

Result<StartUploadResult> HttpMultipartFlow::StartMultipart(const ObjectDescriptor& desc) {
  auto out = data_client_.StartUpload(
      desc.object,
      static_cast<std::uint64_t>(desc.expected_total_size.value_or(0)),
      desc.idempotency_key);
  if (!out.success()) return Result<StartUploadResult>::Failure(out.error());
  StartUploadResult r;
  r.upload_id     = std::move(out.value().upload_id);
  r.max_part_size = out.value().max_part_size;
  return Result<StartUploadResult>::Success(std::move(r));
}

Result<TransferOutcome> HttpMultipartFlow::UploadPart(
    const ObjectDescriptor& desc, const std::string& upload_id,
    std::uint32_t part_number, ConstBufferView buffer) {
  return transfer_path_.PutObjectPart(desc, buffer, upload_id, part_number);
}

Result<IMultipartFlow::CompleteResult> HttpMultipartFlow::CompleteMultipart(
    const std::string& upload_id, const std::vector<PartRef>& parts) {
  std::vector<HttpDataClient::PartEtag> http_parts;
  http_parts.reserve(parts.size());
  for (const auto& p : parts) http_parts.push_back({p.part_number, p.etag, std::nullopt});
  auto out = data_client_.CompleteUpload(upload_id, http_parts);
  if (!out.success()) return Result<CompleteResult>::Failure(out.error());
  return Result<CompleteResult>::Success(
      {out.value().etag, out.value().version, out.value().content_length});
}

Result<bool> HttpMultipartFlow::AbortMultipart(const std::string& upload_id) {
  return data_client_.AbortUpload(upload_id);
}

}  // namespace us3_turbo_access::client
