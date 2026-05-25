#include "core/multipart/multipart_coordinator.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "common/error.h"
#include "common/metrics.h"

namespace us3_turbo_access::gateway::core::multipart {

MultipartCoordinator::MultipartCoordinator(
    backend::IBackend& backend, MultipartStore& store,
    std::size_t max_part_size, std::size_t min_part_size,
    std::shared_ptr<spdlog::logger> logger)
    : backend_(backend),
      store_(store),
      max_part_size_(max_part_size),
      min_part_size_(min_part_size),
      logger_(std::move(logger)) {}

// 创建 multipart upload。分两个 ID：backend_upload_id 给 backend，
// upload_id 暴露给客户端，避免 backend 句柄格式渗透到 wire。
Result<StartUploadResult> MultipartCoordinator::StartUpload(
    const StartUploadParams& params) {
  auto backend_id = backend_.StartMultipart(
      params.bucket, params.object_key, params.expected_total_size);
  if (!backend_id.success()) {
    common::metrics().multipart_fail_total << 1;
    return Result<StartUploadResult>::Failure(backend_id.error());
  }
  MultipartStore::CreateParams cp;
  cp.bucket = params.bucket;
  cp.object_key = params.object_key;
  cp.data_path = params.data_path;
  cp.expected_total_size = params.expected_total_size;
  cp.backend_upload_id = backend_id.value();
  auto upload = store_.Create(cp);
  if (logger_ != nullptr) {
    logger_->info("multipart.start upload_id={} bucket={} key={} backend_id={}",
                  upload->upload_id, params.bucket, params.object_key,
                  backend_id.value());
  }
  StartUploadResult out;
  out.upload_id = upload->upload_id;
  out.max_part_size = max_part_size_;
  common::metrics().multipart_start_total << 1;
  return Result<StartUploadResult>::Success(std::move(out));
}

// 提交 multipart upload。
// 并发保护：state CAS 到 kCompleting 抢占 owner，重复 Complete 被拒。
// 校验：part_number 1..N 连续；非末尾 part >= min_part_size；etag 与记录一致。
Result<ObjectMetadata> MultipartCoordinator::CompleteUpload(
    std::string_view upload_id,
    const std::vector<backend::PartRecord>& parts) {
  common::ScopedLatency latency(common::metrics().multipart_complete_latency_us);
  if (parts.empty()) {
    common::metrics().multipart_fail_total << 1;
    return Result<ObjectMetadata>::Failure(common::MakeError(
        ErrorCode::kBadRequest, "CompleteUpload requires at least one part"));
  }
  auto upload = store_.Find(upload_id);
  if (upload == nullptr) {
    common::metrics().multipart_fail_total << 1;
    return Result<ObjectMetadata>::Failure(common::MakeError(
        ErrorCode::kNotFound, "multipart upload_id not found"));
  }
  std::string backend_id;
  std::map<std::uint32_t, PartProgress> recorded_parts;
  {
    std::scoped_lock lock(upload->mu);
    if (upload->state != State::kActive) {
      common::metrics().multipart_fail_total << 1;
      return Result<ObjectMetadata>::Failure(common::MakeError(
          ErrorCode::kBadRequest, "multipart upload not active"));
    }
    upload->state = State::kCompleting;  // 抢占 owner
    backend_id = upload->backend_upload_id;
    recorded_parts = upload->parts;       // 快照后释放锁再校验
  }

  // RAII：校验/提交失败时把 state 回退到 kActive，避免 upload 卡死。
  struct StateRollbackGuard {
    MultipartUpload* upload;
    bool armed{true};
    ~StateRollbackGuard() {
      if (armed && upload != nullptr) {
        std::scoped_lock lock(upload->mu);
        upload->state = State::kActive;
      }
    }
  } rollback{upload.get()};

  // S3-style validation: part_number 1..N contiguous; size ≥ min_part_size
  // except last; etag present and matches what gateway recorded at WritePart.
  std::vector<backend::PartRecord> sorted = parts;
  std::sort(sorted.begin(), sorted.end(), backend::PartRecordByNumberLess);
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    const auto& pr = sorted[i];
    const std::uint32_t expected_number = static_cast<std::uint32_t>(i + 1);
    if (pr.part_number != expected_number) {
      common::metrics().multipart_fail_total << 1;
      return Result<ObjectMetadata>::Failure(common::MakeError(
          ErrorCode::kBadRequest,
          "part_number sequence must be 1..N contiguous; got " +
              std::to_string(pr.part_number) + " at index " +
              std::to_string(i)));
    }
    if (pr.etag.empty()) {
      common::metrics().multipart_fail_total << 1;
      return Result<ObjectMetadata>::Failure(common::MakeError(
          ErrorCode::kBadRequest,
          "etag must be supplied for part " + std::to_string(pr.part_number)));
    }
    auto rec_it = recorded_parts.find(pr.part_number);
    if (rec_it == recorded_parts.end()) {
      common::metrics().multipart_fail_total << 1;
      return Result<ObjectMetadata>::Failure(common::MakeError(
          ErrorCode::kBadRequest,
          "part " + std::to_string(pr.part_number) +
              " has no recorded data on gateway"));
    }
    if (rec_it->second.etag != pr.etag) {
      common::metrics().multipart_fail_total << 1;
      if (logger_ != nullptr) {
        logger_->warn("multipart.etag_mismatch part={} recorded={} supplied={}",
                      pr.part_number, rec_it->second.etag, pr.etag);
      }
      return Result<ObjectMetadata>::Failure(common::MakeError(
          ErrorCode::kBadRequest,
          "etag mismatch for part " + std::to_string(pr.part_number)));
    }
    const bool is_last = (i + 1 == sorted.size());
    if (!is_last && min_part_size_ != 0 &&
        rec_it->second.size < min_part_size_) {
      common::metrics().multipart_fail_total << 1;
      return Result<ObjectMetadata>::Failure(common::MakeError(
          ErrorCode::kBadRequest,
          "part " + std::to_string(pr.part_number) +
              " smaller than min_part_size (" +
              std::to_string(rec_it->second.size) + " < " +
              std::to_string(min_part_size_) + ")"));
    }
  }
  auto meta = backend_.CompleteMultipart(backend_id, parts);
  if (!meta.success()) {
    common::metrics().multipart_fail_total << 1;
    return meta;
  }
  // Backend commit succeeded — disarm rollback and mark completed.
  rollback.armed = false;
  {
    std::scoped_lock lock(upload->mu);
    upload->state = State::kCompleted;
  }
  store_.Erase(upload->upload_id);
  common::metrics().multipart_complete_total << 1;
  if (logger_ != nullptr) {
    logger_->info("multipart.complete upload_id={} parts={} size={} etag={}",
                  upload->upload_id, parts.size(), meta.value().content_length,
                  meta.value().etag);
  }
  return meta;
}

Result<bool> MultipartCoordinator::AbortUpload(std::string_view upload_id) {
  auto upload = store_.Find(upload_id);
  if (upload == nullptr) {
    return Result<bool>::Success(false);
  }
  std::string backend_id;
  {
    std::scoped_lock lock(upload->mu);
    backend_id = upload->backend_upload_id;
    upload->state = State::kAborted;
  }
  (void)backend_.AbortMultipart(backend_id);
  store_.Erase(upload->upload_id);
  common::metrics().multipart_abort_total << 1;
  if (logger_ != nullptr) {
    logger_->info("multipart.abort upload_id={}", std::string(upload_id));
  }
  return Result<bool>::Success(true);
}

void MultipartCoordinator::AbortBackend(std::string_view backend_upload_id) {
  if (backend_upload_id.empty()) {
    return;
  }
  (void)backend_.AbortMultipart(backend_upload_id);
  common::metrics().multipart_abort_total << 1;
}

Result<std::shared_ptr<MultipartUpload>>
MultipartCoordinator::Lookup(std::string_view upload_id) const {
  auto upload = store_.Find(upload_id);
  if (upload == nullptr) {
    return Result<std::shared_ptr<MultipartUpload>>::Failure(
        common::MakeError(ErrorCode::kNotFound,
                          "multipart upload_id not found"));
  }
  return Result<std::shared_ptr<MultipartUpload>>::Success(std::move(upload));
}

void MultipartCoordinator::RegisterPart(MultipartUpload& upload,
                                        std::uint32_t part_number,
                                        std::uint64_t offset,
                                        std::uint64_t size,
                                        std::string etag) {
  PartProgress p;
  p.part_number = part_number;
  p.offset = offset;
  p.size = size;
  p.etag = std::move(etag);
  const auto now = std::chrono::steady_clock::now();
  std::scoped_lock lock(upload.mu);
  upload.parts[part_number] = std::move(p);
  upload.last_activity_at = now;
  common::metrics().multipart_part_bytes << static_cast<std::int64_t>(size);
}

}  // namespace us3_turbo_access::gateway::core::multipart
