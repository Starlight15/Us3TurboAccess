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
#include "client/src/core/http/http_transfer_path.h"
#include "client/src/core/rdma/rdma_transfer_path.h"
#include "client/src/core/routing/transfer_router.h"
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
  // 注意必须按 data_path 分发：HTTP 的 upload_id 在 HTTP 端独立分配，baidu_std
  // 的 metadata_client 不认；反之亦然。
  if (impl_ && !impl_->upload_id.empty() && impl_->core != nullptr) {
    bool need_abort = false;
    {
      std::scoped_lock lock(impl_->mu);
      need_abort = !impl_->finished;
    }
    if (need_abort) {
      if (impl_->data_path == DataPath::kHttpTcp) {
        (void)impl_->core->http_data_client().AbortUpload(impl_->upload_id);
      } else {
        (void)impl_->core->metadata_client().AbortUpload(impl_->upload_id);
      }
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

// GDS 单 part 实现：现有行为不变。OpenSession 把 expected_size=0
// 透传，gateway 不在 OnSessionOpened 里做整对象 Reserve。
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

  auto registration = core.gds_memory_registry().Register(
      OperationType::kPut, buffer);
  if (!registration.success()) {
    return Result<TransferOutcome>::Failure(registration.error());
  }
  auto open_request = MakeSessionHandshake(core.options(), SessionPlan{
      .operation = OperationType::kPut,
      .request = request,
      .buffer_type = buffer.type,
      .path = DataPath::kGdsCuObject,
  });
  auto open_response =
      core.metadata_client().OpenTransferSession(open_request);
  if (!open_response.success()) {
    return Result<TransferOutcome>::Failure(open_response.error());
  }
  auto session = ImportSession(open_response.value());

  return core.cuobj_client().ExecutePutPart(
      core.options(), core.gds_data_client(), session, request, buffer,
      upload_id, part_number);
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

  Result<TransferOutcome> outcome = [&]() {
    switch (impl_->data_path) {
      case DataPath::kNativeRdma:
        return UploadPartRdma(*impl_->core, impl_->object, checksum_policy,
                                impl_->upload_id, part_number, object_offset, buffer);
      case DataPath::kHttpTcp:
        return UploadPartHttp(*impl_->core, impl_->object, checksum_policy,
                                impl_->upload_id, part_number, object_offset, buffer);
      case DataPath::kGdsCuObject:
      default:
        return UploadPartGds(*impl_->core, impl_->object, checksum_policy,
                              impl_->upload_id, part_number, object_offset, buffer);
    }
  }();
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

// 并发上传多个 part。cuObj 调用被 g_cuobj_global_mu 串行化，
// 并发收益体现在 OpenSession / RegisterPart 等阶段以及 gateway 侧 io_pool。
Result<std::vector<TransferOutcome>>
MultipartUpload::UploadParts(const std::vector<PartSpec>& parts,
                              std::size_t concurrency) {
  if (parts.empty()) {
    return Result<std::vector<TransferOutcome>>::Success({});
  }
  if (concurrency == 0) {
    concurrency = 1;
  }
  if (concurrency > parts.size()) {
    concurrency = parts.size();
  }

  // cuObj/cuFile 需要每个 worker 显式 cudaSetDevice，先捕获主线程的当前 device。
  int main_device = 0;
  (void)cudaGetDevice(&main_device);

  std::vector<TransferOutcome> outcomes(parts.size());
  std::atomic<std::size_t>     next_index{0};
  std::mutex                   error_mu;
  std::optional<Error>         first_error;

  auto worker = [&]() {
    if (cudaSetDevice(main_device) != cudaSuccess) {
      std::scoped_lock lock(error_mu);
      if (!first_error.has_value()) {
        first_error = MakeInvalidArgument(
            "worker cudaSetDevice failed; cannot bind CUDA context");
      }
      return;
    }
    while (true) {
      const auto idx = next_index.fetch_add(1, std::memory_order_acq_rel);
      if (idx >= parts.size()) {
        return;
      }
      {
        std::scoped_lock lock(error_mu);
        if (first_error.has_value()) {
          return;
        }
      }
      const auto& spec = parts[idx];
      auto r = UploadPart(spec.part_number, spec.object_offset, spec.buffer);
      if (!r.success()) {
        std::scoped_lock lock(error_mu);
        if (!first_error.has_value()) {
          first_error = r.error();
        }
      } else {
        outcomes[idx] = std::move(r.value());
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(concurrency);
  for (std::size_t i = 0; i < concurrency; ++i) {
    threads.emplace_back(worker);
  }
  for (auto& t : threads) {
    t.join();
  }
  if (first_error.has_value()) {
    return Result<std::vector<TransferOutcome>>::Failure(*first_error);
  }
  return Result<std::vector<TransferOutcome>>::Success(std::move(outcomes));
}

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

  // 按 data_path 分发：GDS / RDMA 走 control plane（baidu_std）；
  // HTTP 走 HTTP /v1/uploads/{upload_id}/complete。
  Result<CompleteUploadResult> result = [&]() -> Result<CompleteUploadResult> {
    if (impl_->data_path == DataPath::kHttpTcp) {
      std::vector<HttpDataClient::PartEtag> http_parts;
      http_parts.reserve(parts_copy.size());
      for (const auto& p : parts_copy) {
        http_parts.push_back({p.part_number, p.etag});
      }
      auto out = impl_->core->http_data_client().CompleteUpload(
          impl_->upload_id, http_parts);
      if (!out.success()) {
        return Result<CompleteUploadResult>::Failure(out.error());
      }
      CompleteUploadResult r;
      r.etag           = out.value().etag;
      r.version        = out.value().version;
      r.content_length = out.value().content_length;
      return Result<CompleteUploadResult>::Success(std::move(r));
    }
    auto out = impl_->core->metadata_client().CompleteUpload(impl_->upload_id,
                                                              parts_copy);
    if (!out.success()) {
      return Result<CompleteUploadResult>::Failure(out.error());
    }
    CompleteUploadResult r;
    r.etag           = out.value().etag;
    r.version        = out.value().version;
    r.content_length = out.value().content_length;
    return Result<CompleteUploadResult>::Success(std::move(r));
  }();

  // Complete 成功 → finished=true，析构不再 abort。
  // Complete 失败 → best-effort abort 释放 server 端 upload（与 ~MultipartUpload
  // 走同一通道）；但仍把 finished 置 true，避免重复 abort 把用户错误覆盖掉。
  if (!result.success()) {
    if (impl_->data_path == DataPath::kHttpTcp) {
      (void)impl_->core->http_data_client().AbortUpload(impl_->upload_id);
    } else {
      (void)impl_->core->metadata_client().AbortUpload(impl_->upload_id);
    }
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
  Result<bool> out =
      (impl_->data_path == DataPath::kHttpTcp)
          ? impl_->core->http_data_client().AbortUpload(impl_->upload_id)
          : impl_->core->metadata_client().AbortUpload(impl_->upload_id);
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
  return core_->metadata_client().HeadObject(object);
}

Result<TransferOutcome> Client::GetObject(const RequestOptions& request,
                                          MutableBufferView buffer) const {
  if (!core_->initialized()) {
    return Result<TransferOutcome>::Failure(MakeNotInitialized("Client"));
  }
  return core_->transfer_router().GetObject(request, buffer);
}

Result<TransferOutcome> Client::PutObject(const RequestOptions& request,
                                          ConstBufferView buffer) const {
  if (!core_->initialized()) {
    return Result<TransferOutcome>::Failure(MakeNotInitialized("Client"));
  }
  return core_->transfer_router().PutObject(request, buffer);
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

  if (path == DataPath::kHttpTcp) {
    // HTTP 路径：StartUpload 直接走 HTTP /v1/uploads/{bucket}/{key}，
    // 不走 control plane baidu_std。
    auto out = core_->http_data_client().StartUpload(
        object, static_cast<std::uint64_t>(expected_total_size), idempotency_key);
    if (!out.success()) {
      return Result<MultipartUpload>::Failure(out.error());
    }
    impl->upload_id     = std::move(out.value().upload_id);
    impl->max_part_size = out.value().max_part_size;
  } else {
    StartUploadOptions opts;
    opts.object              = object;
    opts.expected_total_size = expected_total_size;
    opts.data_path           = path;  // 仅 GDS / RDMA
    opts.idempotency_key     = idempotency_key;
    auto out = core_->metadata_client().StartUpload(opts);
    if (!out.success()) {
      return Result<MultipartUpload>::Failure(out.error());
    }
    impl->upload_id     = std::move(out.value().upload_id);
    impl->max_part_size = out.value().max_part_size;
  }
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
