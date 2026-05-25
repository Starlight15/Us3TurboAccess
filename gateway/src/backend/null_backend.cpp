#include "backend/null_backend.h"

namespace us3_turbo_access::gateway::backend {

namespace {

ObjectMetadata MakeEmptyMeta() {
  ObjectMetadata meta;
  meta.content_length = 0;
  meta.etag = "\"null\"";
  meta.version = "null";
  return meta;
}

}  // namespace

Result<ObjectMetadata> NullBackend::Head(std::string_view, std::string_view) {
  return Result<ObjectMetadata>::Success(MakeEmptyMeta());
}

Result<std::size_t> NullBackend::Read(std::string_view, std::string_view,
                                      std::uint64_t, std::span<std::byte>) {
  return Result<std::size_t>::Success(0U);
}

Result<ObjectMetadata> NullBackend::Write(std::string_view, std::string_view,
                                          std::span<const std::byte>) {
  return Result<ObjectMetadata>::Success(MakeEmptyMeta());
}

Result<ObjectMetadata> NullBackend::WriteRange(std::string_view,
                                               std::string_view,
                                               std::uint64_t,
                                               std::span<const std::byte>,
                                               std::optional<std::size_t>) {
  return Result<ObjectMetadata>::Success(MakeEmptyMeta());
}

Result<ObjectMetadata> NullBackend::Reserve(std::string_view, std::string_view,
                                            std::size_t) {
  return Result<ObjectMetadata>::Success(MakeEmptyMeta());
}

Result<std::string> NullBackend::StartMultipart(std::string_view,
                                                std::string_view,
                                                std::optional<std::size_t>) {
  return Result<std::string>::Success("null-mpu");
}

Result<std::string> NullBackend::WritePart(std::string_view, std::uint32_t,
                                           std::uint64_t,
                                           std::span<const std::byte>) {
  return Result<std::string>::Success("\"null-part\"");
}

Result<ObjectMetadata> NullBackend::CompleteMultipart(
    std::string_view, const std::vector<PartRecord>&) {
  return Result<ObjectMetadata>::Success(MakeEmptyMeta());
}

Result<bool> NullBackend::AbortMultipart(std::string_view) {
  return Result<bool>::Success(true);
}

}  // namespace us3_turbo_access::gateway::backend
