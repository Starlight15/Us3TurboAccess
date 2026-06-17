#include "client/src/core/client/multipart_upload_impl.h"

#include <utility>

#include "client/src/control/metadata_client.h"
#include "client/src/core/async/client_executor.h"
#include "client/src/core/client/client_core.h"
#include "client/src/core/common/errors.h"
#include "client/src/core/metrics/client_metrics.h"
#include "client/src/core/routing/transfer_router.h"
#include "client/src/core/upload/upload_coordinator.h"
#include "client/src/transports/gds/gds_memory_registry.h"

namespace us3_turbo_access::client {

// ============================================================
//  匿名 namespace
// ============================================================

namespace {

template <typename T>
std::future<Result<T>> MakeReadyFuture(Result<T> r) {
  std::promise<Result<T>> p;
  p.set_value(std::move(r));
  return p.get_future();
}

}  // namespace

// ============================================================
//  构造 / 析构
// ============================================================

Client::Client(ClientOptions options)
    : core_(std::make_unique<ClientCore>(std::move(options))) {}

Client::~Client() = default;

// ============================================================
//  生命周期
// ============================================================

Result<bool> Client::Initialize() { return core_->Initialize(); }

void Client::Shutdown() { core_->Shutdown(); }

bool Client::initialized() const { return core_->initialized(); }

const PlatformCapabilities& Client::capabilities() const { return core_->capabilities(); }

// ============================================================
//  同步操作
// ============================================================

Result<ObjectMetadata> Client::HeadObject(const ObjectId& object) const {
  if (!core_->initialized()) {
    return Result<ObjectMetadata>::Failure(MakeNotInitialized("Client"));
  }
  ScopedTransferMetric metric(ScopedTransferMetric::Op::kHead);
  auto r = core_->metadata_client().HeadObject(object);
  if (r.success()) metric.MarkSuccess();
  return r;
}

Result<TransferOutcome> Client::GetObject(const RequestOptions& request,
                                          MutableBufferView buffer) const {
  if (!core_->initialized()) {
    return Result<TransferOutcome>::Failure(MakeNotInitialized("Client"));
  }
  ScopedTransferMetric metric(ScopedTransferMetric::Op::kGet);
  auto r = core_->transfer_router().GetObject(request, buffer);
  if (r.success()) {
    metric.MarkSuccess();
    metric.SetBytes(static_cast<std::int64_t>(r.value().bytes_transferred));
    metric.SetDataPath(r.value().selected_path);  // C.4 per-path 拆分
  }
  return r;
}

Result<TransferOutcome> Client::PutObject(const RequestOptions& request,
                                          ConstBufferView buffer) const {
  if (!core_->initialized()) {
    return Result<TransferOutcome>::Failure(MakeNotInitialized("Client"));
  }
  ScopedTransferMetric metric(ScopedTransferMetric::Op::kPut,
                                static_cast<std::int64_t>(buffer.size));
  auto r = core_->transfer_router().PutObject(request, buffer);
  if (r.success()) {
    metric.MarkSuccess();
    metric.SetDataPath(r.value().selected_path);  // C.4 per-path 拆分
  }
  return r;
}

// ============================================================
//  异步操作
// ============================================================

std::future<Result<ObjectMetadata>> Client::HeadObjectAsync(
    const ObjectId& object) const {
  if (!core_->initialized()) {
    return MakeReadyFuture(Result<ObjectMetadata>::Failure(
        MakeNotInitialized("Client")));
  }
  return core_->async_executor().Submit(
      [core = core_.get(), object]() -> Result<ObjectMetadata> {
        return core->metadata_client().HeadObject(object);
      });
}

std::future<Result<TransferOutcome>> Client::GetObjectAsync(
    const RequestOptions& request, MutableBufferView buffer) const {
  if (!core_->initialized()) {
    return MakeReadyFuture(Result<TransferOutcome>::Failure(
        MakeNotInitialized("Client")));
  }
  return core_->async_executor().Submit(
      [core = core_.get(), request, buffer]() -> Result<TransferOutcome> {
        return core->transfer_router().GetObject(request, buffer);
      });
}

std::future<Result<TransferOutcome>> Client::PutObjectAsync(
    const RequestOptions& request, ConstBufferView buffer) const {
  if (!core_->initialized()) {
    return MakeReadyFuture(Result<TransferOutcome>::Failure(
        MakeNotInitialized("Client")));
  }
  return core_->async_executor().Submit(
      [core = core_.get(), request, buffer]() -> Result<TransferOutcome> {
        return core->transfer_router().PutObject(request, buffer);
      });
}

// ============================================================
//  分片上传入口
// ============================================================

// 职责：SDK 对外入口；创建 MultipartUpload handle，填充 impl，路由到 coordinator。
Status Client::StartUpload(const ObjectId& object, MultipartUpload* out,
                           std::size_t expected_total_size,
                           const std::string& idempotency_key) {
  if (out == nullptr) {
    return Status::FromError(MakeInvalidArgument("StartUpload: out is null"));
  }
  if (!core_->initialized()) {
    return Status::FromError(MakeNotInitialized("Client"));
  }
  ObjectDescriptor desc;
  desc.object               = object;
  desc.data_path            = core_->options().data_path;
  desc.expected_total_size  = expected_total_size;
  desc.idempotency_key      = idempotency_key;

  auto impl = std::make_unique<MultipartUpload::Impl>();
  impl->data_path = desc.data_path;

  auto& flow = core_->upload_coordinator().SelectFlow(desc.data_path);
  if (auto st = flow.CreateSession(desc, &impl->session); !st.ok()) return st;

  *out = MultipartUpload(std::move(impl));
  return Status::Ok();
}

// ============================================================
//  GPU buffer 管理
// ============================================================

Result<bool> Client::RegisterDeviceBuffer(void* ptr, std::size_t size) {
  if (!core_->initialized()) {
    return Result<bool>::Failure(MakeNotInitialized("Client"));
  }
  if (core_->options().data_path != DataPath::kGdsCuObject) {
    // 非 GDS 通路无需 cuObj descriptor 注册；保持 idempotent 友好。
    return Result<bool>::Success(true);
  }
  return core_->gds_memory_registry().RegisterBuffer(ptr, size);
}

Result<bool> Client::UnregisterDeviceBuffer(void* ptr) {
  if (!core_->initialized()) {
    return Result<bool>::Failure(MakeNotInitialized("Client"));
  }
  if (core_->options().data_path != DataPath::kGdsCuObject) {
    return Result<bool>::Success(true);
  }
  return core_->gds_memory_registry().UnregisterBuffer(ptr);
}

}  // namespace us3_turbo_access::client
