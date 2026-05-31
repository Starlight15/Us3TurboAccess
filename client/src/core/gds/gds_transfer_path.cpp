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

[[nodiscard]] Error MakeGdsUnsupportedError() {
  return MakeUnsupportedPath(DataPath::kGdsCuObject,
                             "The current environment does not support the GDS channel");
}

// 注册显存并向 gateway 协商出一个 TransferSession。
// memory_registry 验证 buffer 类型 / 非空，但 descriptor 字段已从 wire 协议
// 移除（gateway 用不到），所以这里只走校验。
template <typename BufferView>
[[nodiscard]] Result<TransferSession> OpenSession(const GdsContext& context, OperationType op,
                                                   const RequestOptions& request,
                                                   BufferView buffer) {
  auto registration = context.memory_registry.Register(op, buffer);
  if (!registration.success()) {
    return Result<TransferSession>::Failure(registration.error());
  }

  auto open_request = MakeSessionHandshake(context.options, SessionPlan{
      .operation = op,
      .request = request,
      .buffer_type = buffer.type,
      .path = DataPath::kGdsCuObject,
  });

  auto open_response = context.metadata_client.OpenTransferSession(open_request);
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

DataPath GdsTransferPath::path() const { return DataPath::kGdsCuObject; }

bool GdsTransferPath::available() const { return caps_.cuobject_available; }

// GET 入口：按 request.length / parallel_get_threshold / chunks 选并发或单连接。
// 与 HttpTransferPath::GetObject 路由逻辑完全对称。
Result<TransferOutcome> GdsTransferPath::GetObject(const RequestOptions& request,
                                                   MutableBufferView buffer) const {
  if (!available()) {
    return Result<TransferOutcome>::Failure(MakeGdsUnsupportedError());
  }

  // 并发条件：1) 已知 length；2) length >= threshold；3) chunks >= 2；4) executor 可用。
  // 任一不满足走单连接 GetObjectSingle。
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
    const RequestOptions& request, MutableBufferView buffer) const {
  auto session = OpenSession(context_, OperationType::kGet, request, buffer);
  if (!session.success()) {
    return Result<TransferOutcome>::Failure(session.error());
  }
  return context_.cuobj_client.ExecuteGet(context_.options, context_.data_client,
                                            session.value(), request, buffer);
}

// 并发分片 GET：[offset, offset+length) 切 N 块，每块独立 OpenSession+ExecuteGet。
// 子任务在 ClientExecutor (bthread) 上跑；任一失败立即返错。
Result<TransferOutcome> GdsTransferPath::GetObjectParallel(
    const RequestOptions& request, MutableBufferView buffer,
    ClientExecutor& executor) const {
  const std::uint64_t total = *request.length;
  const std::size_t   k     = std::min<std::size_t>(
      context_.options.gds.parallel_get_chunks,
      static_cast<std::size_t>(total));
  if (k <= 1) return GetObjectSingle(request, buffer);

  // 均匀切：前 (total % k) 块多 1 字节。
  const std::uint64_t base = total / k;
  const std::uint64_t rem  = total % k;
  std::uint64_t       cursor_off = request.offset;
  std::uint64_t       cursor_buf = 0;

  using SubResult = std::pair<Result<TransferOutcome>, std::uint64_t>;
  std::vector<std::future<SubResult>> futs;
  futs.reserve(k);

  for (std::size_t i = 0; i < k; ++i) {
    const std::uint64_t len = base + (i < rem ? 1 : 0);
    const std::uint64_t off = cursor_off;
    void* dst = static_cast<std::byte*>(buffer.data) + cursor_buf;
    MutableBufferView sub{.data = dst, .size = len, .type = buffer.type};

    // 构造子 RequestOptions：offset/length 收窄到本块，progress_callback 不传，
    // 由父调用方在合并时统一回调。
    RequestOptions sub_req = request;
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
                                  .data_path = DataPath::kGdsCuObject});
    }
  }

  TransferOutcome outcome;
  outcome.selected_path     = DataPath::kGdsCuObject;
  outcome.transfer_status   = "completed";
  outcome.etag              = std::move(etag);
  outcome.version           = std::move(version);
  outcome.request_id        = std::move(request_id);
  outcome.session_id        = std::move(session_id);
  outcome.gateway_id        = std::move(gateway_id);
  outcome.bytes_transferred = total_bytes;
  return Result<TransferOutcome>::Success(std::move(outcome));
}

Result<TransferOutcome> GdsTransferPath::PutObject(const RequestOptions& request,
                                                   ConstBufferView buffer) const {
  if (!available()) {
    return Result<TransferOutcome>::Failure(MakeGdsUnsupportedError());
  }

  // PUT 幂等覆写：retryable 错误自动重试，每次重做 OpenSession + ExecutePut。
  // 失败时 best-effort abort server 端 session，避免重试导致 server 端
  // session 积压（GDS 通路没有像 RDMA 那样的专用 AbortSession 数据面，
  // 通过 ControlPlaneService::AbortSession RPC 触发 MarkFailed）。
  return RetryIfRetryable(DefaultRetryPolicy(), [&]() -> Result<TransferOutcome> {
    auto session = OpenSession(context_, OperationType::kPut, request, buffer);
    if (!session.success()) {
      return Result<TransferOutcome>::Failure(session.error());
    }
    const std::string session_id = session.value().meta.session_id;
    auto outcome = context_.cuobj_client.ExecutePut(context_.options, context_.data_client,
                                                      session.value(), request, buffer);
    if (!outcome.success()) {
      // Best-effort 让 server 立即 mark failed，无需等 TTL sweep。
      (void)context_.metadata_client.AbortSession(session_id);
    }
    return outcome;
  });
}

// Multipart 单 part：与整对象 PutObject 同流程，但 expected_size=0 跳过
// gateway 端 OnSessionOpened 的整对象 Reserve；最终走 cuobj.ExecutePutPart
// 落 part_etag。逻辑搬自原 client.cpp::UploadPartGds，保持行为一致。
Result<TransferOutcome> GdsTransferPath::PutObjectPart(
    const RequestOptions& request, ConstBufferView buffer,
    const std::string& upload_id, std::uint32_t part_number) const {
  if (!available()) {
    return Result<TransferOutcome>::Failure(MakeGdsUnsupportedError());
  }

  // 单 part 幂等（同 part_number 覆盖），可重试。
  // 失败时 best-effort abort 旧 session（与 PutObject 同款治理）。
  return RetryIfRetryable(DefaultRetryPolicy(), [&]() -> Result<TransferOutcome> {
    // expected_size = 0：OpenSession 中 length 不带，gateway 不做整对象 Reserve。
    auto registration =
        context_.memory_registry.Register(OperationType::kPut, buffer);
    if (!registration.success()) {
      return Result<TransferOutcome>::Failure(registration.error());
    }

    auto open_request = MakeSessionHandshake(context_.options, SessionPlan{
        .operation = OperationType::kPut,
        .request = request,
        .buffer_type = buffer.type,
        .path = DataPath::kGdsCuObject,
    });
    auto open_response = context_.metadata_client.OpenTransferSession(open_request);
    if (!open_response.success()) {
      return Result<TransferOutcome>::Failure(open_response.error());
    }
    auto session = ImportSession(open_response.value());
    const std::string session_id = session.meta.session_id;

    auto outcome = context_.cuobj_client.ExecutePutPart(
        context_.options, context_.data_client, session, request, buffer,
        upload_id, part_number);
    if (!outcome.success()) {
      (void)context_.metadata_client.AbortSession(session_id);
    }
    return outcome;
  });
}

}  // namespace us3_turbo_access::client
