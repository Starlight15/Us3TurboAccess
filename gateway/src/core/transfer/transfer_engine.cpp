#include "core/transfer/transfer_engine.h"

#include <utility>

namespace us3_turbo_access::gateway::core {

TransferEngine::TransferEngine(backend::IBackend& backend,
                               std::shared_ptr<spdlog::logger> logger)
    : backend_(backend),
      logger_(logger),
      http_(std::make_unique<data_path::http::HttpExecutor>(backend, std::move(logger))) {}

Result<TransferReport> TransferEngine::HttpGet(std::string_view bucket,
                                               std::string_view key,
                                               std::uint64_t offset,
                                               std::uint64_t length,
                                               HttpResponseSink sink) {
  return http_->Get(bucket, key, offset, length, sink);
}

Result<TransferReport> TransferEngine::HttpPut(std::string_view bucket,
                                               std::string_view key,
                                               std::span<const std::byte> body) {
  return http_->Put(bucket, key, body);
}

Result<ObjectMetadata> TransferEngine::Head(std::string_view bucket,
                                            std::string_view key) {
  return backend_.Head(bucket, key);
}

Result<ObjectMetadata> TransferEngine::Reserve(std::string_view bucket,
                                               std::string_view key,
                                               std::size_t total_size) {
  return backend_.Reserve(bucket, key, total_size);
}

}  // namespace us3_turbo_access::gateway::core
