#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <spdlog/logger.h>

#include "backend/backend.h"
#include "us3_turbo_access/gateway/result.h"

namespace brpc {
class Controller;
}  // namespace brpc

namespace us3_turbo_access::gateway::data_path::http {

/**
 * @brief Outcome description recorded for a single transfer.
 */
struct TransferReport {
  std::uint64_t  bytes_transferred{0};
  ObjectMetadata meta;
};

/**
 * @brief Editable view of the brpc controller used by the HTTP frontend.
 *
 * The executor writes the response attachment in place; status codes and
 * headers are set by the caller (frontend) after consulting the returned
 * report.
 */
struct HttpResponseSink {
  brpc::Controller* controller{nullptr};
};

/**
 * @brief HTTP / TCP server-side data-path executor.
 *
 * Owns the HTTP-specific transfer logic: chunked GET streamed into the brpc
 * controller, PUT body persisted to the backend. The TransferEngine routes
 * HTTP-path requests through this class; the HTTP frontend never talks to it
 * directly.
 */
class HttpExecutor {
 public:
  HttpExecutor(backend::IBackend& backend,
               std::shared_ptr<spdlog::logger> logger);

  HttpExecutor(const HttpExecutor&) = delete;
  HttpExecutor& operator=(const HttpExecutor&) = delete;

  /**
   * @brief Streams object content from the backend into the HTTP response.
   *
   * The body is appended to the controller's attachment using a chunked loop
   * to bound memory usage even for large reads.
   */
  [[nodiscard]] Result<TransferReport>
    Get(std::string_view bucket, std::string_view key, std::uint64_t offset,
        std::uint64_t length, HttpResponseSink sink);

  /**
   * @brief Persists the HTTP request body into the backend.
   */
  [[nodiscard]] Result<TransferReport>
    Put(std::string_view bucket, std::string_view key,
        std::span<const std::byte> body);

 private:
  backend::IBackend&              backend_;
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace us3_turbo_access::gateway::data_path::http
