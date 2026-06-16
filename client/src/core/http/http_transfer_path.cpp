#include "client/src/core/http/http_transfer_path.h"

#include <algorithm>
#include <atomic>
#include <future>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "client/src/core/async/client_executor.h"
#include "client/src/core/common/crc32c_helper.h"
#include "client/src/core/common/errors.h"
#include "client/src/core/http/future_utils.h"
#include "client/src/data/http_crc32c.h"

namespace us3_turbo_access::client {

namespace {

constexpr DataPath kPath = DataPath::kHttpTcp;

/*
 * HTTP 路径只接 kHostRegular：与 GDS（kCudaDevice）/ RDMA（kHostPinned）三档
 * 明确分流；让用户在 path / buffer 类型不匹配时尽早拿到清晰错误，而不是走到
 * data client 才报。
 */
[[nodiscard]] Result<bool> Preflight(bool available, BufferType buffer_type) {
  // 调用 TransferPath::CommonPreflight 让 HTTP/RDMA/GDS 三通路共享同款入口
  // 校验风格 + 错误消息（B.5）。
  return HttpTransferPath::CommonPreflight(
      kPath, "HTTP", available, buffer_type, BufferType::kHostRegular);
}

}  // namespace

HttpTransferPath::HttpTransferPath(const ClientOptions& options,
                                     HttpDataClient& data_client,
                                     ExecutorProvider executor_provider)
    : options_(options),
      data_client_(data_client),
      executor_provider_(std::move(executor_provider)) {}

void HttpTransferPath::SetAvailable(bool available) noexcept {
  available_.store(available, std::memory_order_release);
}

DataPath HttpTransferPath::path() const { return kPath; }

bool HttpTransferPath::available() const {
  return available_.load(std::memory_order_acquire);
}

Result<TransferOutcome> HttpTransferPath::GetObject(
    const RequestOptions& request, MutableBufferView buffer) const {
  auto pre = Preflight(available(), buffer.type);
  if (!pre.success()) return Result<TransferOutcome>::Failure(pre.error());

  // 选并发 / 单连接路径。同时满足以下条件才走并发：
  //   1) request.length 已设：不知道总长就无法均匀切 → fallback single；
  //   2) length >= parallel_get_threshold（默认 16 MiB）：小对象并发收益负数（多 RPC 开销）；
  //   3) chunks >= 2：单 chunk 没意义；
  //   4) executor_provider 返回可用 ClientExecutor：线程池未就绪 → fallback single。
  // 任一条件不满足都走 GetObjectSingle（行为等价 V1）。
  const auto threshold = options_.http.parallel_get_threshold;
  const auto chunks    = options_.http.parallel_get_chunks;
  ClientExecutor* exec = executor_provider_ ? executor_provider_() : nullptr;
  if (request.length.has_value() && *request.length >= threshold &&
      chunks >= 2 && exec != nullptr) {
    return GetObjectParallel(request, buffer, *exec);
  }
  return GetObjectSingle(request, buffer);
}

Result<TransferOutcome> HttpTransferPath::GetObjectSingle(
    const RequestOptions& request, MutableBufferView buffer) const {
  auto report = data_client_.GetObject(request.object, request.offset,
                                          request.length, buffer);
  if (!report.success()) return Result<TransferOutcome>::Failure(report.error());

  // HTTP 路径不走 control plane session：request_id / session_id / gateway_id 留空。
  TransferOutcome outcome;
  outcome.selected_path     = kPath;
  outcome.transfer_status   = "completed";
  outcome.etag              = report.value().meta.etag;
  outcome.version           = report.value().meta.version;
  outcome.bytes_transferred = report.value().bytes;
  outcome.server_crc32c     = report.value().server_crc32c;
  // 完成时回调一次（单连接 GET 中间无进度）。
  if (request.progress_callback) {
    request.progress_callback({.bytes_completed = report.value().bytes,
                                .bytes_total = report.value().bytes,
                                .data_path = kPath});
  }
  return Result<TransferOutcome>::Success(std::move(outcome));
}

/*
 * 并发 Ranged GET：把 [offset, offset+length) 切成 chunks 块，每块用一个
 * 子 RPC 同步写到 buffer.data 的对应偏移位置。各任务写不同 byte range，
 * 不会数据竞争。
 *
 * 错误处理 fail-fast：按提交顺序 get future，任一 sub 出错立刻返回错误，
 * 剩余 future 在函数返回 / vector 析构时被丢弃；剩余 worker 仍会跑完 HTTP
 * 一次调用，但其 packaged_task 内 promise 写完后随之析构（合法行为）。
 * HTTP/1.1 单连接没有 cancellation point，无法真正中断 inflight 子请求。
 */
Result<TransferOutcome> HttpTransferPath::GetObjectParallel(
    const RequestOptions& request, MutableBufferView buffer,
    ClientExecutor& executor) const {
  const std::uint64_t total = *request.length;
  const std::size_t   k     = std::min<std::size_t>(
      options_.http.parallel_get_chunks, static_cast<std::size_t>(total));
  if (k <= 1) return GetObjectSingle(request, buffer);

  // 均匀切：前 (total % k) 块多 1 字节。
  const std::uint64_t base = total / k;
  const std::uint64_t rem  = total % k;
  std::uint64_t       cursor_off = request.offset;
  std::uint64_t       cursor_buf = 0;

  // 用 pair 而非自定义 struct：Result<T> 没默认构造，pair 走值传递避开问题。
  using SubResult = std::pair<Result<HttpDataClient::GetReport>, std::uint64_t>;
  std::vector<std::future<SubResult>> futs;
  futs.reserve(k);

  for (std::size_t i = 0; i < k; ++i) {
    const std::uint64_t len = base + (i < rem ? 1 : 0);
    const std::uint64_t off = cursor_off;
    void* dst = static_cast<std::byte*>(buffer.data) + cursor_buf;
    MutableBufferView sub{.data = dst, .size = len, .type = buffer.type};
    auto fut = executor.Submit([this, obj = request.object, off, len, sub]() {
      return SubResult{
          data_client_.GetObject(obj, off, std::optional<std::uint64_t>(len),
                                  sub),
          len};
    });
    futs.push_back(std::move(fut));
    cursor_off += len;
    cursor_buf += len;
  }

  std::uint64_t        total_bytes = 0;
  std::string          etag, version;
  auto results = when_all(futs);
  for (auto& sr : results) {
    auto& r = sr.first;
    const auto expected = sr.second;
    if (!r.success()) {
      return Result<TransferOutcome>::Failure(r.error());
    }
    if (r.value().bytes != expected) {
      return Result<TransferOutcome>::Failure(MakeTransportFailure(
          "parallel GET sub-range short read", kPath, "", false));
    }
    total_bytes += r.value().bytes;
    if (etag.empty())    etag    = r.value().meta.etag;
    if (version.empty()) version = r.value().meta.version;
    // 每完成一个 sub-range 回调一次，让上层看到累积进度。
    if (request.progress_callback) {
      request.progress_callback({.bytes_completed = total_bytes,
                                  .bytes_total = total,
                                  .data_path = kPath});
    }
  }

  TransferOutcome outcome;
  outcome.selected_path     = kPath;
  outcome.transfer_status   = "completed";
  outcome.etag              = std::move(etag);
  outcome.version           = std::move(version);
  outcome.bytes_transferred = total_bytes;
  outcome.server_crc32c     = std::nullopt;  // 并行路径无法校验 CRC
  return Result<TransferOutcome>::Success(std::move(outcome));
}

Result<TransferOutcome> HttpTransferPath::PutObject(
    const RequestOptions& request, ConstBufferView buffer) const {
  auto pre = Preflight(available(), buffer.type);
  if (!pre.success()) return Result<TransferOutcome>::Failure(pre.error());

  // 单段 PUT 大小兜底：超过 put_single_max_bytes（默认 5 GiB，与 S3 服务端硬限对齐）
  // 直接拒绝，避免把超大 buffer 推到 TCP 再被 gateway 413。建议改走分片上传。
  if (buffer.size > options_.http.put_single_max_bytes) {
    return Result<TransferOutcome>::Failure(MakeError(
        ErrorCode::kPayloadTooLarge,
        "PUT body " + std::to_string(buffer.size) +
            " exceeds put_single_max_bytes " +
            std::to_string(options_.http.put_single_max_bytes) +
            "; use StartUpload + UploadPart for multipart upload",
        /*retryable=*/false));
  }

  // V2：本期默认 send_crc32c=true，算一次 CRC32C 注入 header 让 server 端校验。
  auto crc = ComputeClientCrc32c(buffer, options_.http.send_crc32c);
  auto report = data_client_.PutObject(request.object, buffer, crc);
  if (!report.success()) return Result<TransferOutcome>::Failure(report.error());

  TransferOutcome outcome;
  outcome.selected_path     = kPath;
  outcome.transfer_status   = "completed";
  outcome.etag              = report.value().meta.etag;
  outcome.version           = report.value().meta.version;
  outcome.bytes_transferred = report.value().bytes;
  outcome.server_crc32c     = report.value().server_crc32c;
  // 单段 PUT 完成时回调一次（HTTP 同步 RPC 中间没有进度可报）。
  if (request.progress_callback) {
    request.progress_callback({.bytes_completed = report.value().bytes,
                                .bytes_total = buffer.size,
                                .data_path = kPath});
  }
  return Result<TransferOutcome>::Success(std::move(outcome));
}

Result<TransferOutcome> HttpTransferPath::PutObjectPart(
    const RequestOptions& request, ConstBufferView buffer,
    const std::string& upload_id, std::uint32_t part_number) const {
  auto pre = Preflight(available(), buffer.type);
  if (!pre.success()) return Result<TransferOutcome>::Failure(pre.error());
  if (upload_id.empty() || part_number == 0) {
    return Result<TransferOutcome>::Failure(MakeInvalidArgument(
        "HTTP PutObjectPart 需要非空 upload_id 与 part_number >= 1"));
  }

  std::optional<std::uint32_t> crc;
  if (options_.http.send_crc32c && buffer.size > 0 && buffer.data != nullptr) {
    crc = Crc32c(std::span<const std::byte>(
        static_cast<const std::byte*>(buffer.data), buffer.size));
  }

  auto part = data_client_.UploadPart(upload_id, part_number, buffer, crc);
  if (!part.success()) return Result<TransferOutcome>::Failure(part.error());

  // request.object 在 HTTP 路径里不参与 part 路径（part 已经按 upload_id 路由），
  // 但保留进 outcome 给上层做日志/追踪。
  (void)request;
  TransferOutcome outcome;
  outcome.selected_path     = kPath;
  outcome.transfer_status   = "completed";
  outcome.etag              = part.value().etag;   // part_etag
  outcome.version           = "";  // PutPart 无 version，由最终 CompleteMultipart 返回
  outcome.bytes_transferred = buffer.size;
  outcome.server_crc32c     = part.value().server_crc32c;
  // Multipart 单 part 完成回调；上层 MultipartUpload::UploadParts 串起多 part 进度。
  if (request.progress_callback) {
    request.progress_callback({.bytes_completed = buffer.size,
                                .bytes_total = buffer.size,
                                .data_path = kPath});
  }
  return Result<TransferOutcome>::Success(std::move(outcome));
}

}  // namespace us3_turbo_access::client
