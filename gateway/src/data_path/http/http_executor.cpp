#include "data_path/http/http_executor.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <brpc/controller.h>
#include <butil/iobuf.h>

namespace us3_turbo_access::gateway::data_path::http {

namespace {

constexpr std::size_t kChunkBytes = 1U * 1024U * 1024U;  // 1 MiB streaming chunk

}  // namespace

HttpExecutor::HttpExecutor(backend::IBackend& backend,
                           std::shared_ptr<spdlog::logger> logger)
    : backend_(backend), logger_(std::move(logger)) {}

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
  return Result<TransferReport>::Success(std::move(report));
}

Result<TransferReport> HttpExecutor::Put(std::string_view bucket,
                                         std::string_view key,
                                         std::span<const std::byte> body) {
  auto write = backend_.Write(bucket, key, body);
  if (!write.success()) {
    return Result<TransferReport>::Failure(write.error());
  }
  TransferReport report;
  report.bytes_transferred = body.size();
  report.meta = write.value();
  return Result<TransferReport>::Success(std::move(report));
}

}  // namespace us3_turbo_access::gateway::data_path::http
