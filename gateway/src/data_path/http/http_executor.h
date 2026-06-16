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
 *
 * Multipart part 的业务编排（Lookup / RegisterPart）已迁至
 * HttpMultipartPathHandler；本类只保留纯数据写入能力。
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
   * @brief Write a single multipart part's data to the backend.
   *
   * This is a low-level write operation with CRC verification but NO
   * multipart orchestration (no Lookup, no RegisterPart). The caller
   * (HttpMultipartPathHandler) is responsible for the upload lifecycle.
   *
   * @param backend_upload_id  Backend-internal upload ID (resolved by caller)
   * @param part_number        1-based part number
   * @param body               Part payload (IOBuf, zero-copy)
   * @param expected_crc32c    Optional CRC32C for end-to-end verification
   * @return TransferReport with backend-returned etag, bytes written, CRC32C.
   */
  [[nodiscard]] Result<TransferReport>
    WritePartData(std::string_view backend_upload_id,
                  std::uint32_t part_number,
                  const butil::IOBuf& body,
                  std::optional<std::uint32_t> expected_crc32c);

 private:
  backend::IBackend&              backend_;
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace us3_turbo_access::gateway::data_path::http
