#include "client/src/core/client/multipart_upload_impl.h"

#include <atomic>
#include <optional>
#include <thread>
#include <utility>

#include <cuda_runtime.h>

#include "client/src/core/common/errors.h"
#include "client/src/core/metrics/client_metrics.h"

namespace us3_turbo_access::client {

// ============================================================
//  匿名 namespace：Impl 状态辅助 + UploadParts 并发模型
// ============================================================

namespace {

// --- Impl 状态读写辅助 ---
// File-local helpers：只做受锁保护的 Impl 状态读写（finished / parts /
// bytes_committed）。不承载任何协议逻辑。目的是把主流程中零散的
// scoped_lock + 字段访问收束成具名操作，使主流程更直线。
// Helper templates avoid naming MultipartUpload::Impl (private nested type);
// template argument is deduced at the call site inside member functions.

template <typename ImplT>
bool ShouldBestEffortAbortOnDestruct(ImplT* impl) {
  if (impl == nullptr) return false;
  if (impl->session == nullptr) return false;
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
  IMultipartSession::PartRef pc;
  pc.part_number = part_number;
  pc.etag        = std::move(etag);
  std::scoped_lock lock(impl.mu);
  impl.parts.push_back(std::move(pc));
  impl.bytes_committed += bytes;
}

template <typename ImplT>
Result<std::vector<IMultipartSession::PartRef>> SnapshotPartsForComplete(ImplT& impl) {
  std::scoped_lock lock(impl.mu);
  if (impl.finished) {
    return Result<std::vector<IMultipartSession::PartRef>>::Failure(
        MakeInvalidArgument("multipart upload already finalized"));
  }
  return Result<std::vector<IMultipartSession::PartRef>>::Success(impl.parts);
}

template <typename ImplT>
void MarkUploadFinished(ImplT& impl) {
  std::scoped_lock lock(impl.mu);
  impl.finished = true;
}

// --- UploadParts 并发模型 ---

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

// ============================================================
//  构造 / 析构 / move
// ============================================================

MultipartUpload::MultipartUpload() = default;

MultipartUpload::MultipartUpload(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

MultipartUpload::~MultipartUpload() {
  if (!ShouldBestEffortAbortOnDestruct(impl_.get())) return;
  (void)impl_->session->Abort();
}

MultipartUpload::MultipartUpload(MultipartUpload&&) noexcept = default;
MultipartUpload& MultipartUpload::operator=(MultipartUpload&&) noexcept = default;

// ============================================================
//  访问器
// ============================================================

const std::string& MultipartUpload::upload_id() const noexcept {
  return impl_->session->upload_id();
}

std::size_t MultipartUpload::max_part_size() const noexcept {
  return impl_->session->max_part_size();
}

void MultipartUpload::set_checksum_policy(std::string policy) {
  std::scoped_lock lock(impl_->mu);
  impl_->checksum_policy = std::move(policy);
}

// ============================================================
//  单 part 上传
// ============================================================

// 上传单个 part：开 session → 走 path 对应数据面 → 记录 (part_number, etag)。
Result<TransferOutcome> MultipartUpload::UploadPart(std::uint32_t part_number,
                                                    std::uint64_t object_offset,
                                                    ConstBufferView buffer) {
  if (buffer.size == 0) {
    return Result<TransferOutcome>::Failure(
        MakeInvalidArgument("UploadPart buffer is empty"));
  }
  if (impl_->session->max_part_size() != 0 &&
      buffer.size > impl_->session->max_part_size()) {
    return Result<TransferOutcome>::Failure(
        MakeInvalidArgument("part exceeds gateway max_part_size"));
  }

  // 在同一个锁区间内完成 finished 检查 + checksum_policy 快照，消除
  // set_checksum_policy 与无锁 copy 之间的数据竞争。
  std::string checksum_policy;
  {
    std::scoped_lock lock(impl_->mu);
    if (impl_->finished) {
      return Result<TransferOutcome>::Failure(
          MakeInvalidArgument("multipart upload already finalized"));
    }
    checksum_policy = impl_->checksum_policy;
  }

  ScopedTransferMetric metric(ScopedTransferMetric::Op::kUploadPart,
                                static_cast<std::int64_t>(buffer.size),
                                impl_->data_path);
  auto outcome = impl_->session->UploadPart(
      part_number, object_offset, checksum_policy, buffer);
  if (outcome.success()) metric.MarkSuccess();
  if (!outcome.success()) {
    return outcome;
  }

  RecordUploadedPart(*impl_, part_number, outcome.value().etag, buffer.size);
  return outcome;
}

// ============================================================
//  并发上传
// ============================================================

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
    // 任一 part 失败时自动 abort multipart session，避免已上传 part 泄漏。
    // Best-effort：Abort 失败仍返回原始错误。
    (void)Abort();
    return Result<std::vector<TransferOutcome>>::Failure(*shared.first_error);
  }
  return Result<std::vector<TransferOutcome>>::Success(std::move(outcomes));
}

// ============================================================
//  生命周期终止
// ============================================================

Result<CompleteUploadResult> MultipartUpload::Complete() {
  auto snapshot = SnapshotPartsForComplete(*impl_);
  if (!snapshot.success()) {
    return Result<CompleteUploadResult>::Failure(snapshot.error());
  }

  auto coord_out = impl_->session->Complete(snapshot.value());
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
    (void)impl_->session->Abort();
  }
  MarkUploadFinished(*impl_);
  return result;
}

Result<bool> MultipartUpload::Abort() {
  if (IsUploadFinished(*impl_)) {
    return Result<bool>::Success(true);
  }
  Result<bool> out = impl_->session->Abort();
  MarkUploadFinished(*impl_);
  return out;
}

}  // namespace us3_turbo_access::client
