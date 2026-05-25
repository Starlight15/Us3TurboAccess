#include "backend/null_index_store.h"

#include <sstream>
#include <string>

namespace us3_turbo_access::gateway::backend {

Result<ObjectMetadata> NullIndexStore::Head(std::string_view, std::string_view) {
  ObjectMetadata meta;
  meta.content_length = 0;
  meta.etag = "\"null\"";
  meta.version = "null";
  return Result<ObjectMetadata>::Success(meta);
}

Result<bool> NullIndexStore::PutObjectIndex(std::string_view, std::string_view,
                                            std::size_t, std::string_view,
                                            std::string_view) {
  return Result<bool>::Success(true);
}

Result<bool> NullIndexStore::DeleteObjectIndex(std::string_view,
                                                std::string_view) {
  return Result<bool>::Success(true);
}

Result<std::string> NullIndexStore::StartMultipart(
    std::string_view, std::string_view, std::optional<std::size_t>) {
  const auto seq = seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
  std::ostringstream oss;
  oss << "null-mpu-" << seq;
  return Result<std::string>::Success(oss.str());
}

Result<bool> NullIndexStore::AbortMultipart(std::string_view) {
  return Result<bool>::Success(true);
}

Result<ObjectMetadata> NullIndexStore::CommitMultipart(
    std::string_view, std::string_view, std::string_view,
    std::size_t content_length, std::string_view etag,
    std::string_view version) {
  ObjectMetadata meta;
  meta.content_length = content_length;
  meta.etag = std::string(etag);
  meta.version = std::string(version);
  return Result<ObjectMetadata>::Success(std::move(meta));
}

}  // namespace us3_turbo_access::gateway::backend
