#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <spdlog/logger.h>

#include "backend/backend.h"
#include "data_path/http/http_executor.h"
#include "us3_turbo_access/gateway/result.h"

namespace us3_turbo_access::gateway::core {

using TransferReport = data_path::http::TransferReport;
using HttpResponseSink = data_path::http::HttpResponseSink;

/**
 * @brief Routes path-agnostic transfer requests to the right data-path
 *        executor and exposes shared backend operations (Head / Reserve).
 *
 * The HTTP path is owned by an internal @ref data_path::http::HttpExecutor;
 * the GDS path is owned by the runtime and reached through the control plane
 * directly. M2 will plug a native-RDMA executor in alongside.
 */
class TransferEngine {
 public:
  TransferEngine(backend::IBackend& backend,
                 std::shared_ptr<spdlog::logger> logger);

  /**
   * @brief Streams object content from the backend into the HTTP response.
   */
  [[nodiscard]] Result<TransferReport>
    HttpGet(std::string_view bucket, std::string_view key,
            std::uint64_t offset, std::uint64_t length,
            HttpResponseSink sink);

  /**
   * @brief Persists the HTTP request body into the backend.
   */
  [[nodiscard]] Result<TransferReport>
    HttpPut(std::string_view bucket, std::string_view key,
            std::span<const std::byte> body);

  /**
   * @brief Issues a HEAD-style metadata query.
   */
  [[nodiscard]] Result<ObjectMetadata>
    Head(std::string_view bucket, std::string_view key);

  /**
   * @brief Pre-sizes the backing object for chunked uploads (GDS PUT).
   */
  [[nodiscard]] Result<ObjectMetadata>
    Reserve(std::string_view bucket, std::string_view key,
            std::size_t total_size);

 private:
  backend::IBackend&                            backend_;
  std::shared_ptr<spdlog::logger>               logger_;
  std::unique_ptr<data_path::http::HttpExecutor> http_;
};

}  // namespace us3_turbo_access::gateway::core
