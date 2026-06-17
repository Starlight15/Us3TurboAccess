#include "client/src/core/upload/multipart_session_base.h"

namespace us3_turbo_access::client {

MultipartSessionBase::MultipartSessionBase(
    ObjectId object, std::string upload_id,
    std::size_t max_part_size, bool set_length)
    : object_(std::move(object)),
      upload_id_(std::move(upload_id)),
      max_part_size_(max_part_size),
      set_length_(set_length) {}

Result<TransferOutcome> MultipartSessionBase::UploadPart(
    std::uint32_t part_number, std::uint64_t object_offset,
    const std::string& checksum_policy, ConstBufferView buffer) {
  RequestOptions request;
  request.object          = object_;
  request.offset          = object_offset;
  request.checksum_policy = checksum_policy;
  if (set_length_) {
    request.length = buffer.size;
  }
  return DoPutObjectPart(request, buffer, upload_id_, part_number);
}

}  // namespace us3_turbo_access::client
