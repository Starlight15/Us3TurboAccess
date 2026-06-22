#include "client/src/core/gds/gds_transfer_path.h"

#include <algorithm>
#include <future>
#include <utility>
#include <vector>

#include "client/src/core/async/client_executor.h"
#include "client/src/core/common/errors.h"
#include "client/src/core/contracts/request_builder.h"
#include "client/src/core/routing/retry_policy.h"

namespace us3_turbo_access::client {
namespace {

// cuFile 最优对齐粒度：前 k-1 块按 4KB 对齐提升 DMA 性能（避免 unaligned access）
constexpr std::uint64_t kGpuPageSize = 4096;

[[nodiscard]] Error MakeGdsUnsupportedError() {
  return MakeUnsupportedPath(DataFlow::GPUDirect,
                             "The current environment does not support the GDS channel");
}

// B.5 共享 Preflight：GDS 要求 buffer.type=kCudaDevice。
[[nodiscard]] Result<bool> PreflightGds(bool available, BufferType buffer_type) {
  return GdsTransferPath::CommonPreflight(
      DataFlow::GPUDirect, "GDS (requires kCudaDevice GPU buffer)",
      available, buffer_type, BufferType::kCudaDevice);
}

template <typename BufferView>
[[nodiscard]] Result<TransferSession> OpenSessionForGet(
    const GdsContext& context, const GetObjectRequest& request, BufferView buffer) {
  auto registration = context.memory_registry.Register(OperationType::kGet, buffer);
  if (!registration.success()) {
    return Result<TransferSession>::Failure(registration.error());
  }
  auto open_request = MakeSessionHandshake(context.options, SessionPlan{
      .operation = OperationType::kGet,
      .object = request.object,
      .offset = request.offset,
      .length = request.length,
      .timeout = request.timeout,
      .buffer_type = buffer.type,
      .path = DataFlow::GPUDirect,
  });
  auto open_response = context.metadata_client.RpcOpenTransferSession(open_request);
  if (!open_response.success()) {
    return Result<TransferSession>::Failure(open_response.error());
  }
  return Result<TransferSession>::Success(ImportSession(open_response.value()));
}

template <typename BufferView>
[[nodiscard]] Result<TransferSession> OpenSessionForPut(
    const GdsContext& context, const PutObjectRequest& request, BufferView buffer) {
  auto registration = context.memory_registry.Register(OperationType::kPut, buffer);
  if (!registration.success()) {
    return Result<TransferSession>::Failure(registration.error());
  }
  auto open_request = MakeSessionHandshake(context.options, SessionPlan{
      .operation = OperationType::kPut,
      .object = request.object,
      .timeout = request.timeout,
      .idempotency_key = request.idempotency_key,
      .buffer_type = buffer.type,
      .path = DataFlow::GPUDirect,
  });
  auto open_response = context.metadata_client.RpcOpenTransferSession(open_request);
  if (!open_response.success()) {
    return Result<TransferSession>::Failure(open_response.error());
  }
  return Result<TransferSession>::Success(ImportSession(open_response.value()));
}

}  // namespace

GdsTransferPath::GdsTransferPath(const PlatformCapabilities& caps,
                                  const GdsContext& context,
                                  ExecutorProvider executor_provider)
    : caps_(caps), context_(context),
      executor_provider_(std::move(executor_provider)) {}

DataFlow GdsTransferPath::path() const { return DataFlow::GPUDirect; }

bool GdsTransferPath::available() const { return caps_.cuobject_available; }

Result<TransferOutcome> GdsTransferPath::GetObject(const GetObjectRequest& request,
                                                   MutableBufferView buffer) const {
  auto pre = PreflightGds(available(), buffer.type);
  if (!pre.success()) return Result<TransferOutcome>::Failure(pre.error());

  const auto threshold = context_.options.gds.parallel_get_threshold;
  const auto chunks    = context_.options.gds.parallel_get_chunks;
  ClientExecutor* exec = executor_provider_ ? executor_provider_() : nullptr;
  if (request.length.has_value() && *request.length >= threshold &&
      chunks >= 2 && exec != nullptr) {
    return GetObjectParallel(request, buffer, *exec);
  }
  return GetObjectSingle(request, buffer);
}

Result<TransferOutcome> GdsTransferPath::GetObjectSingle(
    const GetObjectRequest& request, MutableBufferView buffer) const {
  auto session = OpenSessionForGet(context_, request, buffer);
  if (!session.success()) {
    return Result<TransferOutcome>::Failure(session.error());
  }
  return context_.cuobj_client.ExecuteGet(context_.options, context_.data_client,
                                            session.value(), request, buffer);
}

Result<TransferOutcome> GdsTransferPath::GetObjectParallel(
    const GetObjectRequest& request, MutableBufferView buffer,
    ClientExecutor& executor) const {
  const std::uint64_t total = *request.length;
  const std::size_t   k     = std::min<std::size_t>(
      context_.options.gds.parallel_get_chunks,
      static_cast<std::size_t>(total));
  if (k <= 1) return GetObjectSingle(request, buffer);

  const std::uint64_t base_aligned = (total / k / kGpuPageSize) * kGpuPageSize;
  const std::uint64_t aligned_total = base_aligned * k;
  const std::uint64_t rem_bytes = total - aligned_total;

  std::uint64_t cursor_off = request.offset;
  std::uint64_t cursor_buf = 0;

  using SubResult = std::pair<Result<TransferOutcome>, std::uint64_t>;
  std::vector<std::future<SubResult>> futs;
  futs.reserve(k);

  for (std::size_t i = 0; i < k; ++i) {
    const std::uint64_t len = (i == k - 1) ? (base_aligned + rem_bytes) : base_aligned;
    if (len == 0) continue;

    const std::uint64_t off = cursor_off;
    void* dst = static_cast<std::byte*>(buffer.data) + cursor_buf;
    MutableBufferView sub{.data = dst, .size = len, .type = buffer.type};

    GetObjectRequest sub_req = request;
    sub_req.offset = off;
    sub_req.length = len;
    sub_req.progress_callback = nullptr;

    auto fut = executor.Submit([this, sub_req, sub, len]() {
      return SubResult{ GetObjectSingle(sub_req, sub), len };
    });
    futs.push_back(std::move(fut));
    cursor_off += len;
    cursor_buf += len;
  }

  std::uint64_t total_bytes = 0;
  std::string   etag, version, request_id, session_id, gateway_id;
  for (auto& f : futs) {
    auto sr = f.get();
    auto& r = sr.first;
    if (!r.success()) {
      return Result<TransferOutcome>::Failure(r.error());
    }
    total_bytes += r.value().bytes_transferred;
    if (etag.empty())       etag       = r.value().etag;
    if (version.empty())    version    = r.value().version;
    if (request_id.empty()) request_id = r.value().request_id;
    if (session_id.empty()) session_id = r.value().session_id;
    if (gateway_id.empty()) gateway_id = r.value().gateway_id;
    if (request.progress_callback) {
      request.progress_callback({.bytes_completed = total_bytes,
                                  .bytes_total = total,
                                  .data_flow = DataFlow::GPUDirect});
    }
  }

  TransferOutcome outcome;
  outcome.selected_flow     = DataFlow::GPUDirect;
  outcome.transfer_status   = "completed";
  outcome.etag              = std::move(etag);
  outcome.version           = std::move(version);
  outcome.request_id        = std::move(request_id);
  outcome.session_id        = std::move(session_id);
  outcome.gateway_id        = std::move(gateway_id);
  outcome.bytes_transferred = total_bytes;
  return Result<TransferOutcome>::Success(std::move(outcome));
}

Result<TransferOutcome> GdsTransferPath::PutObject(const PutObjectRequest& request,
                                                   ConstBufferView buffer) const {
  auto pre = PreflightGds(available(), buffer.type);
  if (!pre.success()) return Result<TransferOutcome>::Failure(pre.error());

  const auto max_put = context_.options.gds.put_single_max_bytes;
  if (max_put != 0 && buffer.size > max_put) {
    return Result<TransferOutcome>::Failure(MakeError(
        ErrorCode::kPayloadTooLarge,
        "GDS PUT body " + std::to_string(buffer.size) +
            " exceeds gds.put_single_max_bytes " + std::to_string(max_put) +
            "; use multipart upload",
        /*retryable=*/false));
  }

  const auto deadline = std::chrono::steady_clock::now() + context_.options.request_timeout;
  return RetryIfRetryable(DefaultRetryPolicy(), [&]() -> Result<TransferOutcome> {
    if (std::chrono::steady_clock::now() >= deadline)
      return Result<TransferOutcome>::Failure(MakeError(
          ErrorCode::kTimeout, "PutObject retry deadline exceeded", true));

    auto session = OpenSessionForPut(context_, request, buffer);
    if (!session.success()) {
      return Result<TransferOutcome>::Failure(session.error());
    }
    const std::string session_id = session.value().meta.session_id;
    auto outcome = context_.cuobj_client.ExecutePut(context_.options, context_.data_client,
                                                      session.value(), request, buffer);
    if (!outcome.success()) {
      (void)context_.metadata_client.AbortSession(session_id);
    }
    return outcome;
  });
}

Result<TransferOutcome> GdsTransferPath::PutObjectPart(
    const PutObjectRequest& request, ConstBufferView buffer,
    const std::string& upload_id, std::uint32_t part_number) const {
  auto pre = PreflightGds(available(), buffer.type);
  if (!pre.success()) return Result<TransferOutcome>::Failure(pre.error());

  const auto max_put = context_.options.gds.put_single_max_bytes;
  if (max_put != 0 && buffer.size > max_put) {
    return Result<TransferOutcome>::Failure(MakeError(
        ErrorCode::kPayloadTooLarge,
        "GDS UploadPart body " + std::to_string(buffer.size) +
            " exceeds gds.put_single_max_bytes " + std::to_string(max_put),
        /*retryable=*/false));
  }

  const auto deadline = std::chrono::steady_clock::now() + context_.options.request_timeout;
  return RetryIfRetryable(DefaultRetryPolicy(), [&]() -> Result<TransferOutcome> {
    if (std::chrono::steady_clock::now() >= deadline)
      return Result<TransferOutcome>::Failure(MakeError(
          ErrorCode::kTimeout, "PutObjectPart retry deadline exceeded", true));

    auto session = OpenSessionForPut(context_, request, buffer);
    if (!session.success()) {
      return Result<TransferOutcome>::Failure(session.error());
    }
    const std::string session_id = session.value().meta.session_id;
    auto outcome = context_.cuobj_client.ExecutePutPart(
        context_.options, context_.data_client, session.value(), request, buffer,
        upload_id, part_number);
    if (!outcome.success()) {
      (void)context_.metadata_client.AbortSession(session_id);
    }
    return outcome;
  });
}

}  // namespace us3_turbo_access::client
