#include "us3_turbo_access/client/client.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <cuda_runtime.h>

#include "client/src/control/metadata_client.h"
#include "client/src/core/client/client_core.h"
#include "client/src/core/common/errors.h"
#include "client/src/core/contracts/request_builder.h"
#include "client/src/core/routing/transfer_router.h"
#include "client/src/data/gds_data_client.h"
#include "client/src/transports/gds/cuobject_client.h"
#include "client/src/transports/gds/gds_memory_registry.h"

namespace us3_turbo_access::client {

struct MultipartUpload::Impl {
  ClientCore*    core{nullptr};
  ObjectId       object;
  std::string    upload_id;
  std::size_t    max_part_size{0};
  std::string    checksum_policy{"none"};

  std::mutex                  mu;
  std::vector<PartCompletion> parts;
  std::size_t                 bytes_committed{0};
  bool                        finished{false};
};

MultipartUpload::MultipartUpload(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

MultipartUpload::~MultipartUpload() {
  if (impl_ && !impl_->upload_id.empty() && impl_->core != nullptr) {
    bool need_abort = false;
    {
      std::scoped_lock lock(impl_->mu);
      need_abort = !impl_->finished;
    }
    if (need_abort) {
      (void)impl_->core->metadata_client().AbortUpload(impl_->upload_id);
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

// 上传单个 part：开 session → cuObj 推数据 → 记录 (part_number, etag)。
// 每个 part 独立 OpenSession，offset 是对象内绝对偏移。
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

  auto& core = *impl_->core;
  RequestOptions request;
  request.object = impl_->object;
  request.offset = object_offset;
  // request.length 留空：对 multipart part，expected_size 报 0 可避免 gateway
  // 在 OnSessionOpened 里对最终对象做 Reserve（part 实际通过 WritePart 落盘）。
  // cuObj 传输大小走 buffer.size（cuobject_client 内部 fallback），不依赖该字段。
  {
    std::scoped_lock lock(impl_->mu);
    request.checksum_policy = impl_->checksum_policy;
  }

  // 仅做 buffer 类型与非空校验。
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

  auto outcome = core.cuobj_client().ExecutePutPart(
      core.options(), core.gds_data_client(), session, request, buffer,
      impl_->upload_id, part_number);
  if (!outcome.success()) {
    return outcome;
  }
  PartCompletion pc;
  pc.part_number = part_number;
  pc.etag = outcome.value().etag;
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
  auto out = impl_->core->metadata_client().CompleteUpload(impl_->upload_id,
                                                            parts_copy);
  {
    std::scoped_lock lock(impl_->mu);
    impl_->finished = true;
  }
  if (!out.success()) {
    return Result<CompleteUploadResult>::Failure(out.error());
  }
  CompleteUploadResult r;
  r.etag = out.value().etag;
  r.version = out.value().version;
  r.content_length = out.value().content_length;
  return Result<CompleteUploadResult>::Success(std::move(r));
}

Result<bool> MultipartUpload::Abort() {
  {
    std::scoped_lock lock(impl_->mu);
    if (impl_->finished) {
      return Result<bool>::Success(true);
    }
  }
  auto out = impl_->core->metadata_client().AbortUpload(impl_->upload_id);
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

Result<MultipartUpload> Client::StartUpload(const ObjectId& object,
                                            std::size_t expected_total_size,
                                            const std::string& idempotency_key) {
  if (!core_->initialized()) {
    return Result<MultipartUpload>::Failure(MakeNotInitialized("Client"));
  }
  StartUploadOptions opts;
  opts.object = object;
  opts.expected_total_size = expected_total_size;
  opts.data_path = DataPath::kGdsCuObject;
  opts.idempotency_key = idempotency_key;
  auto out = core_->metadata_client().StartUpload(opts);
  if (!out.success()) {
    return Result<MultipartUpload>::Failure(out.error());
  }
  auto impl = std::make_unique<MultipartUpload::Impl>();
  impl->core = core_.get();
  impl->object = object;
  impl->upload_id = std::move(out.value().upload_id);
  impl->max_part_size = out.value().max_part_size;
  return Result<MultipartUpload>::Success(MultipartUpload(std::move(impl)));
}

}  // namespace us3_turbo_access::client
