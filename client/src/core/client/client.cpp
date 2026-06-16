#include "us3_turbo_access/client/client.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <cuda_runtime.h>

#include "client/src/core/async/client_executor.h"
#include "client/src/core/client/client_core.h"
#include "client/src/core/common/errors.h"
#include "client/src/core/contracts/request_builder.h"
#include "client/src/core/gds/gds_transfer_path.h"
#include "client/src/core/http/http_transfer_path.h"
#include "client/src/core/metrics/client_metrics.h"
#include "client/src/core/rdma/rdma_transfer_path.h"
#include "client/src/core/routing/transfer_router.h"
#include "client/src/core/upload/upload_coordinator.h"
#include "client/src/data/gds_data_client.h"
#include "client/src/data/http_data_client.h"
#include "client/src/transports/gds/cuobject_client.h"
#include "client/src/transports/gds/gds_memory_registry.h"

namespace us3_turbo_access::client {

// MultipartUpload::Impl 持有一次 multipart 会话的可变状态。
// flow 指向 UploadCoordinator 拥有的某个具体 flow，生命周期与 Client 绑定。
struct MultipartUpload::Impl {
  IMultipartFlow*    flow{nullptr};  // non-owning, valid for Client lifetime
  ObjectDescriptor   descriptor;
  std::string        upload_id;
  std::size_t        max_part_size{0};

  std::mutex                       mu;
  std::vector<IMultipartFlow::PartRef> parts;
  std::size_t                      bytes_committed{0};
  bool                             finished{false};
};

namespace {

// File-local helpers：只做受锁保护的 Impl 状态读写（finished / parts /
// bytes_committed）。不承载任何协议逻辑。目的是把主流程中零散的
// scoped_lock + 字段访问收束成具名操作，使主流程更直线。
// Helper templates avoid naming MultipartUpload::Impl (private nested type);
// template argument is deduced at the call site inside member functions.
template <typename ImplT>
bool ShouldBestEffortAbortOnDestruct(ImplT* impl) {
  if (impl == nullptr) return false;
  if (impl->upload_id.empty() || impl->flow == nullptr) return false;
  std::scoped_lock lock(impl->mu);
  return !impl->finished;
}

template <typename ImplT>
bool IsUploadFinished(ImplT& impl) {
  std::scoped_lock lock(impl.mu);
  return impl.finished;
}

template <typename ImplT>
void RecordUploadedPart(ImplT& impl, std::uint32_t part_number,
                        std::string etag, std::size_t bytes) {
  IMultipartFlow::PartRef pc;
  pc.part_number = part_number;
  pc.etag        = std::move(etag);
  std::scoped_lock lock(impl.mu);
  impl.parts.push_back(std::move(pc));
  impl.bytes_committed += bytes;
}

template <typename ImplT>
Result<std::vector<IMultipartFlow::PartRef>> SnapshotPartsForComplete(ImplT& impl) {
  std::scoped_lock lock(impl.mu);
  if (impl.finished) {
    return Result<std::vector<IMultipartFlow::PartRef>>::Failure(
        MakeInvalidArgument("multipart upload already finalized"));
  }
  return Result<std::vector<IMultipartFlow::PartRef>>::Success(impl.parts);
}

template <typename ImplT>
void MarkUploadFinished(ImplT& impl) {
  std::scoped_lock lock(impl.mu);
  impl.finished = true;
}

}  // namespace

MultipartUpload::MultipartUpload(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

MultipartUpload::~MultipartUpload() {
  if (!ShouldBestEffortAbortOnDestruct(impl_.get())) return;
  (void)impl_->flow->AbortMultipart(impl_->upload_id);
}

MultipartUpload::MultipartUpload(MultipartUpload&&) noexcept = default;
MultipartUpload& MultipartUpload::operator=(MultipartUpload&&) noexcept = default;

const std::string& MultipartUpload::upload_id() const noexcept {
  return impl_->upload_id;
}

std::size_t MultipartUpload::max_part_size() const noexcept {
  return impl_->max_part_size;
}

void MultipartUpload::set_checksum_policy(std::string policy) {
  std::scoped_lock lock(impl_->mu);
  impl_->descriptor.checksum_policy = std::move(policy);
}

// 上传单个 part：开 session → 走 path 对应数据面 → 记录 (part_number, etag)。
Result<TransferOutcome> MultipartUpload::UploadPart(std::uint32_t part_number,
                                                    std::uint64_t object_offset,
                                                    ConstBufferView buffer) {
  if (buffer.size == 0) {
    return Result<TransferOutcome>::Failure(
        MakeInvalidArgument("UploadPart buffer is empty"));
  }
  if (impl_->max_part_size != 0 && buffer.size > impl_->max_part_size) {
    return Result<TransferOutcome>::Failure(
        MakeInvalidArgument("part exceeds gateway max_part_size"));
  }

  // 在同一个锁区间内完成 finished 检查 + descriptor 快照，消除
  // set_checksum_policy 与无锁 copy 之间的数据竞争。
  ObjectDescriptor part_desc;
  {
    std::scoped_lock lock(impl_->mu);
    if (impl_->finished) {
      return Result<TransferOutcome>::Failure(
          MakeInvalidArgument("multipart upload already finalized"));
    }
    part_desc = impl_->descriptor;
  }
  // 锁外补充本次 part 专有字段，不修改共享 descriptor。
  part_desc.offset = object_offset;

  ScopedTransferMetric metric(ScopedTransferMetric::Op::kUploadPart,
                                static_cast<std::int64_t>(buffer.size),
                                part_desc.data_path);
  auto outcome = impl_->flow->UploadPart(
      part_desc, impl_->upload_id, part_number, buffer);
  if (outcome.success()) metric.MarkSuccess();
  if (!outcome.success()) {
    return outcome;
  }

  RecordUploadedPart(*impl_, part_number, outcome.value().etag, buffer.size);
  return outcome;
}

namespace {

// UploadParts 并发模型的共享状态。固定 worker 数量（= concurrency），每个 worker
// 持续通过 next_index.fetch_add 抢下一个 part，直到越界或检测到 first_error。
// first_error 只做 fail-fast（阻止新 part 开始），不中断已在飞的 part。
// 注意：不要把这里改成"每批 parts 再等待"的 batch 模型，会丧失流水线效果。
struct UploadPartsSharedState {
  MultipartUpload*                     upload;
  const std::vector<MultipartUpload::PartSpec>* parts;
  std::vector<TransferOutcome>*        outcomes;
  std::atomic<std::size_t>             next_index{0};
  mutable std::mutex                   error_mu;
  std::optional<Error>                 first_error;
  int                                  main_device{0};

  bool HasError() const {
    std::scoped_lock lock(error_mu);
    return first_error.has_value();
  }

  void RecordError(Error e) {
    std::scoped_lock lock(error_mu);
    if (!first_error.has_value()) first_error = std::move(e);
  }

  void RecordSuccess(std::size_t idx, TransferOutcome outcome) {
    (*outcomes)[idx] = std::move(outcome);
  }
};

class UploadPartsWorker {
 public:
  explicit UploadPartsWorker(UploadPartsSharedState* shared) noexcept
      : shared_(shared) {}

  // 警告：UploadPart() 是阻塞调用（HTTP 等 TCP ACK、RDMA 等 RMA WRITE 完成、
  // GDS 等 cuFile 返回）。worker 必须运行在独立 std::thread，不能迁移到
  // ClientExecutor / brpc bthread。原因：bthread 阻塞会占住底层 pthread，
  // 8 个并发 worker 足以耗尽 brpc pthread pool，拖垮控制面 RPC 调度。
  // 此约束经过性能回归验证（bthread 版本 RDMA 吞吐从 ~18 GB/s 降至 ~640 MB/s）。
  void operator()() const {
    // cuObj / cuFile 要求每个 worker 入口 cudaSetDevice 绑当前 GPU；非 GDS 路径
    // cudaSetDevice 仍 cheap，做了无害。
    if (cudaSetDevice(shared_->main_device) != cudaSuccess) {
      shared_->RecordError(MakeInvalidArgument(
          "worker cudaSetDevice failed; cannot bind CUDA context"));
      return;
    }
    while (true) {
      const auto idx = shared_->next_index.fetch_add(
          1, std::memory_order_acq_rel);
      if (idx >= shared_->parts->size()) return;
      if (shared_->HasError()) return;  // fail-fast：先到的失败短路其余 worker

      const auto& spec = (*shared_->parts)[idx];
      auto r = shared_->upload->UploadPart(spec.part_number,
                                            spec.object_offset, spec.buffer);
      if (r.success()) {
        shared_->RecordSuccess(idx, std::move(r.value()));
      } else {
        shared_->RecordError(r.error());
      }
    }
  }

 private:
  UploadPartsSharedState* shared_;
};

std::size_t NormalizeUploadPartsConcurrency(std::size_t concurrency,
                                            std::size_t part_count) {
  if (concurrency == 0) return 1;
  if (concurrency > part_count) return part_count;
  return concurrency;
}

int CaptureCurrentCudaDevice() {
  int device = 0;
  (void)cudaGetDevice(&device);
  return device;
}

}  // namespace

// 并发上传多个 part。并发模型：固定 worker 数量 + next_index.fetch_add() 流水线，
// 不是 batch 并发（worker 抢完一个立即抢下一个，无批次边界等待）。
// fail-fast：首个失败阻止后续 part 开始，但已在飞的 part 不被中断，仍 join 等回收。
// std::thread 是有意设计，不是历史遗留——见 UploadPartsWorker::operator() 注释。
Result<std::vector<TransferOutcome>>
MultipartUpload::UploadParts(const std::vector<PartSpec>& parts,
                              std::size_t concurrency) {
  if (parts.empty()) {
    return Result<std::vector<TransferOutcome>>::Success({});
  }
  concurrency = NormalizeUploadPartsConcurrency(concurrency, parts.size());

  std::vector<TransferOutcome> outcomes(parts.size());
  UploadPartsSharedState shared;
  shared.upload      = this;
  shared.parts       = &parts;
  shared.outcomes    = &outcomes;
  shared.main_device = CaptureCurrentCudaDevice();

  std::vector<std::thread> threads;
  threads.reserve(concurrency);
  for (std::size_t i = 0; i < concurrency; ++i) {
    threads.emplace_back(UploadPartsWorker(&shared));
  }
  for (auto& t : threads) t.join();

  if (shared.first_error.has_value()) {
    return Result<std::vector<TransferOutcome>>::Failure(*shared.first_error);
  }
  return Result<std::vector<TransferOutcome>>::Success(std::move(outcomes));
}

Result<CompleteUploadResult> MultipartUpload::Complete() {
  auto snapshot = SnapshotPartsForComplete(*impl_);
  if (!snapshot.success()) {
    return Result<CompleteUploadResult>::Failure(snapshot.error());
  }

  auto coord_out = impl_->flow->CompleteMultipart(impl_->upload_id, snapshot.value());
  Result<CompleteUploadResult> result =
      coord_out.success()
          ? Result<CompleteUploadResult>::Success(CompleteUploadResult{
                std::move(coord_out.value().etag),
                std::move(coord_out.value().version),
                coord_out.value().content_length})
          : Result<CompleteUploadResult>::Failure(coord_out.error());

  // Complete 成功 → finished=true，析构不再 abort。
  // Complete 失败 → best-effort abort 释放 server 端 upload（与 ~MultipartUpload
  // 走同一通道）；但仍把 finished 置 true，避免重复 abort 把用户错误覆盖掉。
  if (!result.success()) {
    (void)impl_->flow->AbortMultipart(impl_->upload_id);
  }
  MarkUploadFinished(*impl_);
  return result;
}

Result<bool> MultipartUpload::Abort() {
  if (IsUploadFinished(*impl_)) {
    return Result<bool>::Success(true);
  }
  Result<bool> out = impl_->flow->AbortMultipart(impl_->upload_id);
  MarkUploadFinished(*impl_);
  return out;
}

Client::Client(ClientOptions options)
    : core_(std::make_unique<ClientCore>(std::move(options))) {}

Client::~Client() = default;

Result<bool> Client::Initialize() { return core_->Initialize(); }

void Client::Shutdown() { core_->Shutdown(); }

bool Client::initialized() const { return core_->initialized(); }

const PlatformCapabilities& Client::capabilities() const { return core_->capabilities(); }

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

namespace {

template <typename T>
std::future<Result<T>> MakeReadyFuture(Result<T> r) {
  std::promise<Result<T>> p;
  p.set_value(std::move(r));
  return p.get_future();
}

}  // namespace

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

// 职责：SDK 对外入口；创建 MultipartUpload handle，填充 impl，路由到 coordinator。
Result<MultipartUpload> Client::StartUpload(const ObjectId& object,
                                            std::size_t expected_total_size,
                                            const std::string& idempotency_key) {
  if (!core_->initialized()) {
    return Result<MultipartUpload>::Failure(MakeNotInitialized("Client"));
  }
  ObjectDescriptor desc;
  desc.object               = object;
  desc.data_path            = core_->options().data_path;
  desc.expected_total_size  = expected_total_size;
  desc.idempotency_key      = idempotency_key;

  auto impl = std::make_unique<MultipartUpload::Impl>();
  impl->descriptor = desc;

  auto& flow = core_->upload_coordinator().SelectFlow(desc.data_path);
  auto out = flow.StartMultipart(desc);
  if (!out.success()) {
    return Result<MultipartUpload>::Failure(out.error());
  }
  impl->flow          = &flow;
  impl->upload_id     = std::move(out.value().upload_id);
  impl->max_part_size = out.value().max_part_size;
  MultipartUpload upload(std::move(impl));
  return Result<MultipartUpload>::Success(std::move(upload));
}

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
