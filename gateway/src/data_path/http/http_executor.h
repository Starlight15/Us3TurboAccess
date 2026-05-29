#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/logger.h>

#include "backend/backend.h"
#include "data_path/data_path_executor.h"
#include "us3_turbo_access/gateway/result.h"

namespace butil {
class IOBuf;
}  // namespace butil

namespace brpc {
class Controller;
}  // namespace brpc

namespace us3_turbo_access::gateway::core::multipart {
class MultipartCoordinator;
}  // namespace us3_turbo_access::gateway::core::multipart

namespace us3_turbo_access::gateway::data_path::http {

/**
 * @brief Outcome description recorded for a single transfer.
 *   crc32c：server 端算出来的 CRC32C（成功路径）。Get 路径暂不填。
 */
struct TransferReport {
  std::uint64_t  bytes_transferred{0};
  ObjectMetadata meta;
  std::uint32_t  crc32c{0};
  bool           has_crc32c{false};
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
               core::multipart::MultipartCoordinator* multipart,
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

  /**
   * 整对象 PUT。expected_crc32c 非空时 server 端做 end-to-end 校验：
   * 不一致返回 kInvalidArgument，不落盘。
   * 返回的 TransferReport.crc32c 是 server 实际算出来的值，可作响应头回 client。
   */
  [[nodiscard]] Result<TransferReport>
    Put(std::string_view bucket, std::string_view key,
        std::span<const std::byte> body,
        std::optional<std::uint32_t> expected_crc32c);

  /**
   * 整对象 PUT（零拷贝版本）：接受 IOBuf，避免 to_string() 内存拷贝。
   */
  [[nodiscard]] Result<TransferReport>
    Put(std::string_view bucket, std::string_view key,
        const butil::IOBuf& body,
        std::optional<std::uint32_t> expected_crc32c);

  /**
   * Multipart 单 part PUT：写到 backend.WritePart，写完登记 part 进度
   * 给 MultipartCoordinator（与 GDS/RDMA 路径对称）。expected_crc32c 同 Put。
   * 返回 part_etag（被 client 收集后传给 CompleteUpload）。
   */
  [[nodiscard]] Result<TransferReport>
    PutPart(std::string_view upload_id, std::uint32_t part_number,
            std::span<const std::byte> body,
            std::optional<std::uint32_t> expected_crc32c);

  /**
   * Multipart 单 part PUT（零拷贝版本）：接受 IOBuf。
   */
  [[nodiscard]] Result<TransferReport>
    PutPart(std::string_view upload_id, std::uint32_t part_number,
            const butil::IOBuf& body,
            std::optional<std::uint32_t> expected_crc32c);

 private:
  backend::IBackend&              backend_;
  core::multipart::MultipartCoordinator* multipart_{nullptr};
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace us3_turbo_access::gateway::data_path::http
