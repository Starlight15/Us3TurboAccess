#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/logger.h>

#include "backend/backend.h"
#include "data_path/data_path_executor.h"
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
 */
struct HttpResponseSink {
  brpc::Controller* controller{nullptr};
};

/**
 * @brief HTTP / TCP server-side data-path executor.
 *
 * 实现 IDataPathExecutor lifecycle（永远 available；endpoint 与 brpc port 由
 * HttpFrontend 自然提供，这里报空）。数据面 Get/Put 直接被 HttpFrontend 调用，
 * 不经 control plane。
 */
class HttpExecutor final : public IDataPathExecutor {
 public:
  HttpExecutor(backend::IBackend& backend,
               std::shared_ptr<spdlog::logger> logger);

  HttpExecutor(const HttpExecutor&) = delete;
  HttpExecutor& operator=(const HttpExecutor&) = delete;

  // IDataPathExecutor
  [[nodiscard]] DataPath kind() const noexcept override {
    return DataPath::kHttpTcp;
  }
  [[nodiscard]] bool available() const override { return true; }
  [[nodiscard]] std::string endpoint() const override { return {}; }
  [[nodiscard]] Result<bool> Start() override {
    return Result<bool>::Success(true);
  }
  void Stop() override {}

  // HTTP-specific data plane
  [[nodiscard]] Result<TransferReport>
    Get(std::string_view bucket, std::string_view key, std::uint64_t offset,
        std::uint64_t length, HttpResponseSink sink);

  [[nodiscard]] Result<TransferReport>
    Put(std::string_view bucket, std::string_view key,
        std::span<const std::byte> body);

 private:
  backend::IBackend&              backend_;
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace us3_turbo_access::gateway::data_path::http
