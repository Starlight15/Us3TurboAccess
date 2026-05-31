#include "us3_turbo_access/client/client.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <cuda_runtime.h>

#include "client/src/control/metadata_client.h"
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

struct MultipartUpload::Impl {
  ClientCore*    core{nullptr};
  ObjectId       object;
  std::string    upload_id;
  std::size_t    max_part_size{0};
  std::string    checksum_policy{"none"};
  DataPath       data_path{DataPath::kGdsCuObject};

  std::mutex                  mu;
  std::vector<PartCompletion> parts;
  std::size_t                 bytes_committed{0};
  bool                        finished{false};
};

MultipartUpload::MultipartUpload(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

MultipartUpload::~MultipartUpload() {
  // 析构兜底：用户没 Complete 也没 Abort → best-effort 调对应通道的 AbortUpload。
  // 通过 UploadCoordinator 透明分发，HTTP/GDS/RDMA 各走自己的 abort 路径。
  if (impl_ && !impl_->upload_id.empty() && impl_->core != nullptr) {
    bool need_abort = false;
    {
      std::scoped_lock lock(impl_->mu);
      need_abort = !impl_->finished;
    }
    if (need_abort) {
      (void)impl_->core->upload_coordinator().AbortUpload(
          impl_->data_path, impl_->upload_id);
    }
  }
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
  impl_->checksum_policy = std::move(policy);
}

namespace {

// GDS 单 part：透传到 GdsTransferPath::PutObjectPart（与 RDMA/HTTP 对齐）。
// 内部 expected_size=0 跳过整对象 Reserve，最终走 cuobj.ExecutePutPart。
Result<TransferOutcome> UploadPartGds(ClientCore& core,
                                       const ObjectId& object,
                                       const std::string& checksum_policy,
                                       const std::string& upload_id,
                                       std::uint32_t part_number,
                                       std::uint64_t object_offset,
                                       ConstBufferView buffer) {
  RequestOptions request;
  request.object = object;
  request.offset = object_offset;
  // length 留空：cuObj 走 buffer.size（cuobject_client 内部 fallback），
  // 让 expected_size=0 保留 GDS 原来的 Reserve 跳过行为。
  request.checksum_policy = checksum_policy;
  return core.gds_transfer_path().PutObjectPart(request, buffer, upload_id,
                                                  part_number);
}

// RDMA 单 part 实现：走 RdmaTransferPath::PutObjectPart；
// expected_size = part_size 让 BindSession 阶段能分配 buffer，
// is_multipart_part=true 让 gateway 跳过整对象 Reserve。
Result<TransferOutcome> UploadPartRdma(const ClientCore& core,
                                        const ObjectId& object,
                                        const std::string& checksum_policy,
                                        const std::string& upload_id,
                                        std::uint32_t part_number,
                                        std::uint64_t object_offset,
                                        ConstBufferView buffer) {
  RequestOptions request;
  request.object          = object;
  request.offset          = object_offset;
  request.length          = buffer.size;     // 让 expected_size>0
  request.checksum_policy = checksum_policy;
  return core.rdma_transfer_path().PutObjectPart(request, buffer, upload_id,
                                                   part_number);
}

// HTTP 单 part 实现：与 RDMA 对偶，走 HttpTransferPath::PutObjectPart（内部 PUT
// /v1/uploads/{upload_id}?partNumber=N）。HTTP 不走 control plane session，
// expected_total_size 由 StartUpload 阶段一次性带过去即可。
Result<TransferOutcome> UploadPartHttp(const ClientCore& core,
                                        const ObjectId& object,
                                        const std::string& checksum_policy,
                                        const std::string& upload_id,
                                        std::uint32_t part_number,
                                        std::uint64_t object_offset,
                                        ConstBufferView buffer) {
  RequestOptions request;
  request.object          = object;
  request.offset          = object_offset;
  request.length          = buffer.size;
  request.checksum_policy = checksum_policy;
  return core.http_transfer_path().PutObjectPart(request, buffer, upload_id,
                                                   part_number);
}

}  // namespace

// 上传单个 part：开 session → 走 path 对应数据面 → 记录 (part_number, etag)。
Result<TransferOutcome> MultipartUpload::UploadPart(std::uint32_t part_number,
                                                    std::uint64_t object_offset,
                                                    ConstBufferView buffer) {
  {
    std::scoped_lock lock(impl_->mu);
    if (impl_->finished) {
      return Result<TransferOutcome>::Failure(
          MakeInvalidArgument("multipart upload already finalized"));
    }
  }
  if (buffer.size == 0) {
    return Result<TransferOutcome>::Failure(
        MakeInvalidArgument("UploadPart buffer is empty"));
  }
  if (impl_->max_part_size != 0 && buffer.size > impl_->max_part_size) {
    return Result<TransferOutcome>::Failure(
        MakeInvalidArgument("part exceeds gateway max_part_size"));
  }

  std::string checksum_policy;
  {
    std::scoped_lock lock(impl_->mu);
    checksum_policy = impl_->checksum_policy;
  }

  // 按 data_path 分发：GDS / RDMA 走 control plane + 各自数据面；
  // HTTP 走纯 HTTP UploadPart 路径。
  ScopedTransferMetric metric(ScopedTransferMetric::Op::kUploadPart,
                                static_cast<std::int64_t>(buffer.size));
  Result<TransferOutcome> outcome = Result<TransferOutcome>::Failure(
      MakeInvalidArgument("unreachable"));
  switch (impl_->data_path) {
    case DataPath::kNativeRdma:
      outcome = UploadPartRdma(*impl_->core, impl_->object, checksum_policy,
                                impl_->upload_id, part_number, object_offset, buffer);
      break;
    case DataPath::kHttpTcp:
      outcome = UploadPartHttp(*impl_->core, impl_->object, checksum_policy,
                                impl_->upload_id, part_number, object_offset, buffer);
      break;
    case DataPath::kGdsCuObject:
    default:
      outcome = UploadPartGds(*impl_->core, impl_->object, checksum_policy,
                                impl_->upload_id, part_number, object_offset, buffer);
      break;
  }
  if (outcome.success()) metric.MarkSuccess();
  if (!outcome.success()) {
    return outcome;
  }

  PartCompletion pc;
  pc.part_number = part_number;
  pc.etag        = outcome.value().etag;
  {
    std::scoped_lock lock(impl_->mu);
    impl_->parts.push_back(std::move(pc));
    impl_->bytes_committed += buffer.size;
  }
  return outcome;
}

namespace {

// 多 part 并发上传共享状态：next_index 派发下一笔，first_error fail-fast，
// outcomes 按 index 写回。
struct PartUploadShared {
  MultipartUpload*                     upload;
  const std::vector<MultipartUpload::PartSpec>* parts;
  std::vector<TransferOutcome>*        outcomes;
  std::atomic<std::size_t>             next_index{0};
  std::mutex                           error_mu;
  std::optional<Error>                 first_error;
  int                                  main_device{0};
};

class PartUploadWorker {
 public:
  explicit PartUploadWorker(PartUploadShared* shared) noexcept
      : shared_(shared) {}

  void operator()() const {
    // cuObj / cuFile 要求每个 worker 入口 cudaSetDevice 绑当前 GPU；非 GDS 路径
    // cudaSetDevice 仍 cheap，做了无害。
    if (cudaSetDevice(shared_->main_device) != cudaSuccess) {
      RecordError(MakeInvalidArgument(
          "worker cudaSetDevice failed; cannot bind CUDA context"));
      return;
    }
    while (true) {
      const auto idx = shared_->next_index.fetch_add(
          1, std::memory_order_acq_rel);
      if (idx >= shared_->parts->size()) return;
      if (HasError()) return;  // fail-fast：先到的失败短路其余 worker

      const auto& spec = (*shared_->parts)[idx];
      auto r = shared_->upload->UploadPart(spec.part_number,
                                            spec.object_offset, spec.buffer);
      if (r.success()) {
        (*shared_->outcomes)[idx] = std::move(r.value());
      } else {
        RecordError(r.error());
      }
    }
  }

 private:
  bool HasError() const {
    std::scoped_lock lock(shared_->error_mu);
    return shared_->first_error.has_value();
  }

  void RecordError(Error e) const {
    std::scoped_lock lock(shared_->error_mu);
    if (!shared_->first_error.has_value()) shared_->first_error = std::move(e);
  }

  PartUploadShared* shared_;
};

}  // namespace

// 并发上传多个 part。GDS path 内部已 token 直通可并发；RDMA / HTTP 每 worker
// 独立 RPC。fail-fast：任一 part 失败短路其余在飞 worker（但仍 join 等回收）。
Result<std::vector<TransferOutcome>>
MultipartUpload::UploadParts(const std::vector<PartSpec>& parts,
                              std::size_t concurrency) {
  if (parts.empty()) {
    return Result<std::vector<TransferOutcome>>::Success({});
  }
  if (concurrency == 0) concurrency = 1;
  if (concurrency > parts.size()) concurrency = parts.size();

  std::vector<TransferOutcome> outcomes(parts.size());
  PartUploadShared shared;
  shared.upload   = this;
  shared.parts    = &parts;
  shared.outcomes = &outcomes;
  (void)cudaGetDevice(&shared.main_device);

  std::vector<std::thread> threads;
  threads.reserve(concurrency);
  for (std::size_t i = 0; i < concurrency; ++i) {
    threads.emplace_back(PartUploadWorker(&shared));
  }
  for (auto& t : threads) t.join();

  if (shared.first_error.has_value()) {
    return Result<std::vector<TransferOutcome>>::Failure(*shared.first_error);
  }
  return Result<std::vector<TransferOutcome>>::Success(std::move(outcomes));
}

namespace {

// 把 client 公开 PartCompletion 适配成 UploadCoordinator 内部类型。
std::vector<UploadCoordinator::PartRef> ToCoordinatorParts(
    const std::vector<PartCompletion>& parts) {
  std::vector<UploadCoordinator::PartRef> out;
  out.reserve(parts.size());
  for (const auto& p : parts) {
    out.push_back({p.part_number, p.etag});
  }
  return out;
}

}  // namespace

Result<CompleteUploadResult> MultipartUpload::Complete() {
  std::vector<PartCompletion> parts_copy;
  {
    std::scoped_lock lock(impl_->mu);
    if (impl_->finished) {
      return Result<CompleteUploadResult>::Failure(
          MakeInvalidArgument("multipart upload already finalized"));
    }
    parts_copy = impl_->parts;
  }

  // UploadCoordinator 透明分发：HTTP / GDS / RDMA 各走独立控制面。
  auto coord_out = impl_->core->upload_coordinator().CompleteUpload(
      impl_->data_path, impl_->upload_id, ToCoordinatorParts(parts_copy));
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
    (void)impl_->core->upload_coordinator().AbortUpload(impl_->data_path,
                                                         impl_->upload_id);
  }
  {
    std::scoped_lock lock(impl_->mu);
    impl_->finished = true;
  }
  return result;
}

Result<bool> MultipartUpload::Abort() {
  {
    std::scoped_lock lock(impl_->mu);
    if (impl_->finished) {
      return Result<bool>::Success(true);
    }
  }
  Result<bool> out = impl_->core->upload_coordinator().AbortUpload(
      impl_->data_path, impl_->upload_id);
  {
    std::scoped_lock lock(impl_->mu);
    impl_->finished = true;
  }
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
  if (r.success()) metric.MarkSuccess();
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

Result<MultipartUpload> Client::StartUpload(const ObjectId& object,
                                            std::size_t expected_total_size,
                                            const std::string& idempotency_key) {
  if (!core_->initialized()) {
    return Result<MultipartUpload>::Failure(MakeNotInitialized("Client"));
  }
  const auto path = core_->options().data_path;

  auto impl = std::make_unique<MultipartUpload::Impl>();
  impl->core      = core_.get();
  impl->object    = object;
  impl->data_path = path;

  // UploadCoordinator 透明分发：HTTP 走 /v1/uploads/{bucket}/{key}（HTTP REST），
  // GDS/RDMA 走 ControlPlaneService::StartUpload（baidu_std）。
  // 注意：server 端 MultipartStore 实际是共享的，upload_id 命名空间也共享，
  // 但 A.1 后 server 端在 Complete/Abort 强校验 data_path，跨通路调用会被
  // server 端拒绝（kBadRequest）。client 端这里也按 path 分发避免误用。
  auto out = core_->upload_coordinator().StartUpload(
      path, object, expected_total_size, idempotency_key);
  if (!out.success()) {
    return Result<MultipartUpload>::Failure(out.error());
  }
  impl->upload_id     = std::move(out.value().upload_id);
  impl->max_part_size = out.value().max_part_size;
  return Result<MultipartUpload>::Success(MultipartUpload(std::move(impl)));
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
