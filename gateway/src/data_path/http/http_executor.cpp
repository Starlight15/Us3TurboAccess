#include "data_path/http/http_executor.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <brpc/controller.h>
#include <butil/iobuf.h>

#include "common/crc32c.h"
#include "common/error.h"
#include "core/multipart/multipart_coordinator.h"

namespace us3_turbo_access::gateway::data_path::http {

namespace {

constexpr std::size_t kChunkBytes = 1U * 1024U * 1024U;  // 1 MiB streaming chunk

// CRC32C 校验：only when调用方提供 expected。返回 (实际 crc, mismatch?)。
std::pair<std::uint32_t, bool> VerifyCrc32c(
    std::span<const std::byte> body,
    std::optional<std::uint32_t> expected) {
  const auto actual = common::Crc32c(body);
  const bool mismatch = expected.has_value() && *expected != actual;
  return {actual, mismatch};
}

// CRC32C 校验（IOBuf 版本）：遍历 IOBuf block 计算 CRC。
std::pair<std::uint32_t, bool> VerifyCrc32c(
    const butil::IOBuf& body,
    std::optional<std::uint32_t> expected) {
  std::uint32_t state = common::Crc32cInit();
  const std::size_t num_blocks = body.backing_block_num();

  for (std::size_t i = 0; i < num_blocks; ++i) {
    butil::StringPiece block = body.backing_block(i);
    state = common::Crc32cUpdate(state, block.data(), block.size());
  }

  const std::uint32_t actual = common::Crc32cFinalize(state);
  const bool mismatch = expected.has_value() && *expected != actual;
  return {actual, mismatch};
}

}  // namespace

HttpExecutor::HttpExecutor(backend::IBackend& backend,
                           core::multipart::MultipartCoordinator* multipart,
                           std::shared_ptr<spdlog::logger> logger)
    : backend_(backend), multipart_(multipart), logger_(std::move(logger)) {}

Result<TransferReport> HttpExecutor::Get(std::string_view bucket,
                                         std::string_view key,
                                         std::uint64_t offset,
                                         std::uint64_t length,
                                         HttpResponseSink sink) {
  auto head = backend_.Head(bucket, key);
  if (!head.success()) {
    return Result<TransferReport>::Failure(head.error());
  }

  TransferReport report;
  report.meta = head.value();
  if (length == 0U) {
    return Result<TransferReport>::Success(std::move(report));
  }

  // 边读边算 CRC32C：每个 chunk 一次 backend.Read + Crc32cUpdate + append。
  // 不需要额外遍历 buffer，单遍完成读 + 校验 + 写响应。
  std::uint32_t crc_state = common::Crc32cInit();
  std::vector<std::byte> buffer(std::min<std::size_t>(
      kChunkBytes, static_cast<std::size_t>(length)));
  std::uint64_t cursor = offset;
  std::uint64_t remaining = length;
  while (remaining != 0U) {
    const auto request = std::min<std::uint64_t>(buffer.size(), remaining);
    auto read = backend_.Read(bucket, key, cursor,
                              std::span<std::byte>(buffer.data(), request));
    if (!read.success()) {
      return Result<TransferReport>::Failure(read.error());
    }
    const auto n = read.value();
    if (n == 0U) {
      break;
    }
    // 单遍：算 CRC + 写响应同时进行。
    crc_state = common::Crc32cUpdate(crc_state, buffer.data(), n);
    if (sink.controller != nullptr) {
      sink.controller->response_attachment().append(
          static_cast<const void*>(buffer.data()), n);
    }
    cursor += n;
    remaining -= n;
    report.bytes_transferred += n;
    if (n < request) {
      break;  // backend returned a short read - treat as EOF.
    }
  }
  report.crc32c = common::Crc32cFinalize(crc_state);
  report.has_crc32c = true;
  return Result<TransferReport>::Success(std::move(report));
}

Result<TransferReport> HttpExecutor::Put(std::string_view bucket,
                                         std::string_view key,
                                         std::span<const std::byte> body,
                                         std::optional<std::uint32_t> expected_crc32c) {
  auto [actual_crc, mismatch] = VerifyCrc32c(body, expected_crc32c);
  if (mismatch) {
    return Result<TransferReport>::Failure(common::MakeError(
        ErrorCode::kInvalidArgument,
        "PUT crc32c mismatch: client=" + std::to_string(*expected_crc32c) +
            " server=" + std::to_string(actual_crc)));
  }

  auto write = backend_.Write(bucket, key, body);
  if (!write.success()) {
    return Result<TransferReport>::Failure(write.error());
  }
  TransferReport report;
  report.bytes_transferred = body.size();
  report.meta = write.value();
  report.crc32c = actual_crc;
  report.has_crc32c = true;
  return Result<TransferReport>::Success(std::move(report));
}

Result<TransferReport> HttpExecutor::PutPart(std::string_view upload_id,
                                              std::uint32_t part_number,
                                              std::span<const std::byte> body,
                                              std::optional<std::uint32_t> expected_crc32c) {
  if (multipart_ == nullptr) {
    return Result<TransferReport>::Failure(common::MakeError(
        ErrorCode::kInternal,
        "PutPart called but multipart coordinator not configured"));
  }
  auto [actual_crc, mismatch] = VerifyCrc32c(body, expected_crc32c);
  if (mismatch) {
    return Result<TransferReport>::Failure(common::MakeError(
        ErrorCode::kInvalidArgument,
        "PutPart crc32c mismatch: client=" + std::to_string(*expected_crc32c) +
            " server=" + std::to_string(actual_crc)));
  }

  // upload_id 是 client 拿到的公开 id；backend.WritePart 要的是 backend_upload_id。
  // 与 GDS/RDMA 路径对称（控制面 GdsChunk handler / RdmaExecutor::CommitPart 也是这模式）。
  auto lookup = multipart_->Lookup(upload_id);
  if (!lookup.success()) {
    return Result<TransferReport>::Failure(lookup.error());
  }
  auto upload = lookup.value();

  auto part_etag = backend_.WritePart(upload->backend_upload_id, part_number,
                                        /*offset=*/0, body);
  if (!part_etag.success()) {
    return Result<TransferReport>::Failure(part_etag.error());
  }
  multipart_->RegisterPart(*upload, part_number, /*offset=*/0,
                            static_cast<std::uint64_t>(body.size()),
                            part_etag.value());

  TransferReport report;
  report.bytes_transferred = body.size();
  report.meta.etag = part_etag.value();  // part etag
  report.crc32c = actual_crc;
  report.has_crc32c = true;
  return Result<TransferReport>::Success(std::move(report));
}

// IOBuf 版本的 Put：单遍优化 - 边写边算 CRC
Result<TransferReport> HttpExecutor::Put(std::string_view bucket,
                                         std::string_view key,
                                         const butil::IOBuf& body,
                                         std::optional<std::uint32_t> expected_crc32c) {
  // 单遍优化：边遍历 IOBuf block 边算 CRC 边写入
  std::uint32_t crc_state = common::Crc32cInit();
  const std::size_t total_size = body.size();

  // 先 Reserve 空间
  auto reserve = backend_.Reserve(bucket, key, total_size);
  if (!reserve.success()) {
    return Result<TransferReport>::Failure(reserve.error());
  }

  // 遍历 IOBuf block：边算 CRC 边写入
  std::uint64_t offset = 0;
  const std::size_t num_blocks = body.backing_block_num();
  for (std::size_t i = 0; i < num_blocks; ++i) {
    butil::StringPiece block = body.backing_block(i);

    // 更新 CRC
    crc_state = common::Crc32cUpdate(crc_state, block.data(), block.size());

    // 写入 block
    std::span<const std::byte> span(
        reinterpret_cast<const std::byte*>(block.data()), block.size());
    auto wr = backend_.WriteRange(bucket, key, offset, span, total_size);
    if (!wr.success()) {
      return Result<TransferReport>::Failure(wr.error());
    }
    offset += block.size();
  }

  // 完成 CRC 计算
  const std::uint32_t actual_crc = common::Crc32cFinalize(crc_state);

  // 验证 CRC（如果客户端提供了）
  if (expected_crc32c.has_value() && *expected_crc32c != actual_crc) {
    return Result<TransferReport>::Failure(common::MakeError(
        ErrorCode::kInvalidArgument,
        "PUT crc32c mismatch: client=" + std::to_string(*expected_crc32c) +
            " server=" + std::to_string(actual_crc)));
  }

  TransferReport report;
  report.bytes_transferred = total_size;
  report.meta = reserve.value();  // 使用 Reserve 返回的 meta
  report.crc32c = actual_crc;
  report.has_crc32c = true;
  return Result<TransferReport>::Success(std::move(report));
}

// IOBuf 版本的 PutPart：单遍优化 - 边写边算 CRC
Result<TransferReport> HttpExecutor::PutPart(std::string_view upload_id,
                                              std::uint32_t part_number,
                                              const butil::IOBuf& body,
                                              std::optional<std::uint32_t> expected_crc32c) {
  if (multipart_ == nullptr) {
    return Result<TransferReport>::Failure(common::MakeError(
        ErrorCode::kInternal,
        "PutPart called but multipart coordinator not configured"));
  }

  auto lookup = multipart_->Lookup(upload_id);
  if (!lookup.success()) {
    return Result<TransferReport>::Failure(lookup.error());
  }
  auto upload = lookup.value();

  // 单遍优化：边遍历 IOBuf block 边算 CRC 边写入
  std::uint32_t crc_state = common::Crc32cInit();
  std::uint64_t offset = 0;
  const std::size_t num_blocks = body.backing_block_num();

  for (std::size_t i = 0; i < num_blocks; ++i) {
    butil::StringPiece block = body.backing_block(i);

    // 更新 CRC
    crc_state = common::Crc32cUpdate(crc_state, block.data(), block.size());

    // 写入 block
    std::span<const std::byte> span(
        reinterpret_cast<const std::byte*>(block.data()), block.size());
    auto wr = backend_.WritePart(upload->backend_upload_id, part_number,
                                   offset, span);
    if (!wr.success()) {
      return Result<TransferReport>::Failure(wr.error());
    }
    offset += block.size();
  }

  // 完成 CRC 计算
  const std::uint32_t actual_crc = common::Crc32cFinalize(crc_state);

  // 验证 CRC（如果客户端提供了）
  if (expected_crc32c.has_value() && *expected_crc32c != actual_crc) {
    return Result<TransferReport>::Failure(common::MakeError(
        ErrorCode::kInvalidArgument,
        "PutPart crc32c mismatch: client=" + std::to_string(*expected_crc32c) +
            " server=" + std::to_string(actual_crc)));
  }

  // 使用统一 part etag 格式（替代原 ostringstream 十六进制格式）
  const std::string part_etag = multipart_->GeneratePartEtag(part_number, body.size());

  multipart_->RegisterPart(*upload, part_number, /*offset=*/0,
                            static_cast<std::uint64_t>(body.size()),
                            part_etag);

  TransferReport report;
  report.bytes_transferred = body.size();
  report.meta.etag = part_etag;
  report.crc32c = actual_crc;
  report.has_crc32c = true;
  return Result<TransferReport>::Success(std::move(report));
}

}  // namespace us3_turbo_access::gateway::data_path::http
