#include "backend/null_data_store.h"

namespace us3_turbo_access::gateway::backend {

Result<bool> NullDataStore::Reserve(std::string_view, std::string_view,
                                     std::size_t) {
  return Result<bool>::Success(true);
}

Result<bool> NullDataStore::WriteRange(std::string_view, std::string_view,
                                        std::uint64_t,
                                        std::span<const std::byte>,
                                        std::optional<std::size_t>) {
  return Result<bool>::Success(true);
}

Result<std::size_t> NullDataStore::Read(std::string_view, std::string_view,
                                         std::uint64_t,
                                         std::span<std::byte>) {
  return Result<std::size_t>::Success(0U);
}

Result<bool> NullDataStore::DeleteObject(std::string_view, std::string_view) {
  return Result<bool>::Success(true);
}

Result<bool> NullDataStore::WritePart(std::string_view, std::uint32_t,
                                       std::uint64_t,
                                       std::span<const std::byte>) {
  return Result<bool>::Success(true);
}

Result<bool> NullDataStore::AbortMultipartData(std::string_view) {
  return Result<bool>::Success(true);
}

Result<std::size_t> NullDataStore::AssembleObject(
    std::string_view, std::string_view, std::string_view,
    const std::vector<std::uint32_t>&) {
  return Result<std::size_t>::Success(0U);
}

}  // namespace us3_turbo_access::gateway::backend
