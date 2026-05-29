#include "backend/composite_backend.h"

#include <algorithm>
#include <chrono>
#include <sstream>

#include <butil/iobuf.h>

#include "common/error.h"

namespace us3_turbo_access::gateway::backend {

namespace {

[[nodiscard]] std::uint64_t NowTicks() {
  return static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
}

}  // namespace

CompositeBackend::CompositeBackend(std::unique_ptr<IIndexStore> index_store,
                                    std::unique_ptr<IDataStore> data_store)
    : index_(std::move(index_store)), data_(std::move(data_store)) {}

ObjectMetadata CompositeBackend::MakeMeta(std::size_t size) {
  const auto ticks = NowTicks();
  std::ostringstream etag;
  etag << '"' << std::hex << size << '-' << ticks << '"';
  ObjectMetadata meta;
  meta.content_length = size;
  meta.etag = etag.str();
  meta.version = std::to_string(ticks);
  return meta;
}

std::string CompositeBackend::MakePartEtag(std::uint32_t part_number,
                                            std::size_t size) {
  std::ostringstream oss;
  oss << '"' << std::hex << part_number << '-' << size << '-' << NowTicks()
      << '"';
  return oss.str();
}

// 索引读
Result<ObjectMetadata> CompositeBackend::Head(std::string_view bucket,
                                               std::string_view key) {
  return index_->Head(bucket, key);
}

// 数据读
Result<std::size_t> CompositeBackend::Read(std::string_view bucket,
                                            std::string_view key,
                                            std::uint64_t offset,
                                            std::span<std::byte> dst) {
  return data_->Read(bucket, key, offset, dst);
}

Result<ObjectMetadata> CompositeBackend::Write(std::string_view bucket,
                                                std::string_view key,
                                                std::span<const std::byte> src) {
  return WriteRange(bucket, key, 0, src, src.size());
}

// 单对象写：先写数据，再登记索引。
Result<ObjectMetadata> CompositeBackend::WriteRange(
    std::string_view bucket, std::string_view key, std::uint64_t offset,
    std::span<const std::byte> src, std::optional<std::size_t> total_size) {
  auto wr = data_->WriteRange(bucket, key, offset, src, total_size);
  if (!wr.success()) {
    return Result<ObjectMetadata>::Failure(wr.error());
  }
  const std::size_t final_size =
      total_size.value_or(static_cast<std::size_t>(offset) + src.size());
  auto meta = MakeMeta(final_size);
  auto ir = index_->PutObjectIndex(bucket, key, meta.content_length,
                                    meta.etag, meta.version);
  if (!ir.success()) {
    return Result<ObjectMetadata>::Failure(ir.error());
  }
  return Result<ObjectMetadata>::Success(std::move(meta));
}

// 预占：数据层预扩容 + 索引层登记当前 size。
Result<ObjectMetadata> CompositeBackend::Reserve(std::string_view bucket,
                                                  std::string_view key,
                                                  std::size_t total_size) {
  auto rr = data_->Reserve(bucket, key, total_size);
  if (!rr.success()) {
    return Result<ObjectMetadata>::Failure(rr.error());
  }
  auto meta = MakeMeta(total_size);
  auto ir = index_->PutObjectIndex(bucket, key, meta.content_length,
                                    meta.etag, meta.version);
  if (!ir.success()) {
    return Result<ObjectMetadata>::Failure(ir.error());
  }
  return Result<ObjectMetadata>::Success(std::move(meta));
}

// 索引层建 upload_id；本类同时记下 upload_id → (bucket, key)。
Result<std::string> CompositeBackend::StartMultipart(
    std::string_view bucket, std::string_view key,
    std::optional<std::size_t> total_size_hint) {
  auto ir = index_->StartMultipart(bucket, key, total_size_hint);
  if (!ir.success()) {
    return ir;
  }
  std::scoped_lock lock(mpu_mu_);
  mpu_targets_.emplace(ir.value(),
                        std::make_pair(std::string(bucket), std::string(key)));
  return ir;
}

// part 数据写到数据层；etag 由 composite 层合成（无需 backend 内部细节）。
Result<std::string> CompositeBackend::WritePart(std::string_view upload_id,
                                                 std::uint32_t part_number,
                                                 std::uint64_t offset,
                                                 std::span<const std::byte> src) {
  auto wr = data_->WritePart(upload_id, part_number, offset, src);
  if (!wr.success()) {
    return Result<std::string>::Failure(wr.error());
  }
  return Result<std::string>::Success(MakePartEtag(part_number, src.size()));
}

// 数据层把 parts 拼成最终对象，索引层登记终态。
Result<ObjectMetadata> CompositeBackend::CompleteMultipart(
    std::string_view upload_id, const std::vector<PartRecord>& parts) {
  std::string bucket;
  std::string key;
  {
    std::scoped_lock lock(mpu_mu_);
    auto it = mpu_targets_.find(std::string(upload_id));
    if (it == mpu_targets_.end()) {
      return Result<ObjectMetadata>::Failure(common::MakeError(
          ErrorCode::kNotFound, "upload_id not found in composite mapping"));
    }
    bucket = it->second.first;
    key = it->second.second;
  }

  std::vector<std::uint32_t> ordered;
  ordered.reserve(parts.size());
  for (const auto& pr : parts) {
    ordered.push_back(pr.part_number);
  }
  std::sort(ordered.begin(), ordered.end());

  auto sz = data_->AssembleObject(upload_id, bucket, key, ordered);
  if (!sz.success()) {
    return Result<ObjectMetadata>::Failure(sz.error());
  }
  auto meta = MakeMeta(sz.value());
  auto ir = index_->CommitMultipart(upload_id, bucket, key, meta.content_length,
                                     meta.etag, meta.version);
  if (!ir.success()) {
    return Result<ObjectMetadata>::Failure(ir.error());
  }
  {
    std::scoped_lock lock(mpu_mu_);
    mpu_targets_.erase(std::string(upload_id));
  }
  return Result<ObjectMetadata>::Success(std::move(meta));
}

// 双侧 Abort；映射也一并清理。
Result<bool> CompositeBackend::AbortMultipart(std::string_view upload_id) {
  (void)data_->AbortMultipartData(upload_id);
  (void)index_->AbortMultipart(upload_id);
  std::scoped_lock lock(mpu_mu_);
  mpu_targets_.erase(std::string(upload_id));
  return Result<bool>::Success(true);
}

// IOBuf 零拷贝版本的 Write：遍历 IOBuf block 写入
Result<ObjectMetadata> CompositeBackend::Write(std::string_view bucket,
                                                std::string_view key,
                                                const butil::IOBuf& src) {
  // 先 Reserve 空间
  const std::size_t total_size = src.size();
  auto reserve = data_->Reserve(bucket, key, total_size);
  if (!reserve.success()) {
    return Result<ObjectMetadata>::Failure(reserve.error());
  }

  // 遍历 IOBuf block，逐块写入
  std::uint64_t offset = 0;
  const std::size_t num_blocks = src.backing_block_num();
  for (std::size_t i = 0; i < num_blocks; ++i) {
    butil::StringPiece block = src.backing_block(i);
    std::span<const std::byte> span(
        reinterpret_cast<const std::byte*>(block.data()), block.size());

    auto wr = data_->WriteRange(bucket, key, offset, span, total_size);
    if (!wr.success()) {
      return Result<ObjectMetadata>::Failure(wr.error());
    }
    offset += block.size();
  }

  // 写索引
  auto meta = MakeMeta(total_size);
  auto ir = index_->PutObjectIndex(bucket, key, meta.content_length,
                                    meta.etag, meta.version);
  if (!ir.success()) {
    return Result<ObjectMetadata>::Failure(ir.error());
  }
  return Result<ObjectMetadata>::Success(std::move(meta));
}

// IOBuf 零拷贝版本的 WritePart：遍历 IOBuf block 写入
Result<std::string> CompositeBackend::WritePart(std::string_view upload_id,
                                                 std::uint32_t part_number,
                                                 std::uint64_t base_offset,
                                                 const butil::IOBuf& src) {
  // 遍历 IOBuf block，逐块写入
  std::uint64_t offset = base_offset;
  const std::size_t num_blocks = src.backing_block_num();
  for (std::size_t i = 0; i < num_blocks; ++i) {
    butil::StringPiece block = src.backing_block(i);
    std::span<const std::byte> span(
        reinterpret_cast<const std::byte*>(block.data()), block.size());

    auto wr = data_->WritePart(upload_id, part_number, offset, span);
    if (!wr.success()) {
      return Result<std::string>::Failure(wr.error());
    }
    offset += block.size();
  }

  return Result<std::string>::Success(MakePartEtag(part_number, src.size()));
}

}  // namespace us3_turbo_access::gateway::backend
