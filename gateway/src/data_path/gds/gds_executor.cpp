#include "data_path/gds/gds_executor.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include <openssl/md5.h>

#include <cuobjserver.h>
#include <infiniband/verbs.h>

#include "common/crc32c.h"
#include "common/error.h"
#include "core/metadata/metadata_service.h"
#include "data_path/gds/buffer_pool.h"
#include "data_path/gds/cuobj_resources.h"

namespace us3_turbo_access::gateway::data_path::gds {

namespace {


[[nodiscard]] std::string BuildObjectId(const core::Session& session) {
  return session.bucket + "/" + session.object_key;
}

[[nodiscard]] std::uint64_t ParseRemoteBufferAddress(const std::string& token) {
  const auto colon = token.find(':');
  const std::string hex =
      colon == std::string::npos ? token : token.substr(0, colon);
  if (hex.empty()) {
    return 0;
  }
  return std::strtoull(hex.c_str(), nullptr, 16);
}

[[nodiscard]] std::string DescribeStatus(ibv_wc_status status) {
  const char* description = ibv_wc_status_str(status);
  if (description == nullptr) {
    return std::to_string(static_cast<int>(status));
  }
  return description;
}

[[nodiscard]] std::string Md5Hex(const void* data, std::size_t size) {
  std::array<unsigned char, MD5_DIGEST_LENGTH> digest{};
  MD5(static_cast<const unsigned char*>(data), size, digest.data());
  std::string out;
  out.reserve(MD5_DIGEST_LENGTH * 2 + 2);
  out.push_back('"');
  char buf[3];
  for (auto b : digest) {
    std::snprintf(buf, sizeof(buf), "%02x", b);
    out.append(buf, 2);
  }
  out.push_back('"');
  return out;
}

}  // namespace

GdsExecutor::GdsExecutor(std::string public_host, std::string bind_host,
                         const GdsOptions& opts, backend::IBackend& backend,
                         core::MetadataService& metadata,
                         std::shared_ptr<spdlog::logger> logger)
    : public_host_(std::move(public_host)),
      bind_host_(std::move(bind_host)),
      opts_(opts),
      backend_(backend),
      metadata_(metadata),
      logger_(std::move(logger)) {}

GdsExecutor::~GdsExecutor() { Stop(); }

std::string GdsExecutor::endpoint() const {
  return public_host_ + ":" + std::to_string(opts_.rdma_port);
}

// 起 cuObjServer + PinnedBufferPool；pool 析构需 server 存活，见 Stop()。
Result<bool> GdsExecutor::Start() {
  std::scoped_lock lock(mu_);
  if (server_ != nullptr && server_->isConnected()) {
    return Result<bool>::Success(true);
  }
  auto server = std::make_shared<cuObjServer>(
      bind_host_.c_str(), static_cast<unsigned short>(opts_.rdma_port),
      CUOBJ_PROTO_RDMA_DC_V1);
  if (!server->isConnected()) {
    return Result<bool>::Failure(common::MakeError(
        ErrorCode::kRdmaUnavailable,
        "cuObjServer init failed on " + bind_host_ + ":" +
            std::to_string(opts_.rdma_port)));
  }
  server_ = std::move(server);
  // size class 与 max_per_class 取自 GdsOptions，便于按硬件 / 负载独立调优。
  buffer_pool_ = std::make_shared<PinnedBufferPool>(
      *server_, opts_.buffer_size_classes, opts_.buffer_max_per_class);
  if (logger_ != nullptr) {
    logger_->info("gds: cuObjServer listening on {}:{}", bind_host_,
                  opts_.rdma_port);
  }
  return Result<bool>::Success(true);
}

void GdsExecutor::Stop() {
  // 先销毁 pool（要在 server 还活着时调 deRegisterBuffer），再 reset server。
  std::shared_ptr<PinnedBufferPool> pool_to_destroy;
  {
    std::scoped_lock lock(mu_);
    pool_to_destroy = std::move(buffer_pool_);
  }
  if (pool_to_destroy) {
    pool_to_destroy->Shutdown();
  }
  pool_to_destroy.reset();
  {
    std::scoped_lock lock(mu_);
    server_.reset();
  }
}

bool GdsExecutor::available() const {
  std::scoped_lock lock(mu_);
  return server_ != nullptr && server_->isConnected();
}

Result<bool> GdsExecutor::OnSessionOpened(const core::Session& session) {
  if (!available()) {
    return Result<bool>::Failure(common::MakeError(
        ErrorCode::kRdmaUnavailable,
        "gds-cuobject service is not available on gateway"));
  }
  if (session.op == OperationType::kPut && session.expected_size != 0U) {
    auto reserved = metadata_.Reserve(
        session.bucket, session.object_key,
        static_cast<std::size_t>(session.expected_size));
    if (!reserved.success()) {
      return Result<bool>::Failure(reserved.error());
    }
  }
  return Result<bool>::Success(true);
}

Result<std::shared_ptr<cuObjServer>> GdsExecutor::GetServer() const {
  std::scoped_lock lock(mu_);
  if (server_ == nullptr || !server_->isConnected()) {
    return Result<std::shared_ptr<cuObjServer>>::Failure(
        common::MakeError(ErrorCode::kRdmaUnavailable,
                     "cuObjServer not available"));
  }
  return Result<std::shared_ptr<cuObjServer>>::Success(server_);
}

// GET：backend → pinned buffer → RDMA-WRITE 到 client GPU。单次 ≤ 1 GiB。
// 在 pinned buffer 阶段计算 CRC32C 让 client 做 end-to-end 校验。
Result<GdsExecutor::GetChunkOutcome> GdsExecutor::GetChunk(
    const core::Session& session, const std::string& rdma_token,
    std::uint64_t object_offset, std::uint64_t length) {
  using Outcome = GdsExecutor::GetChunkOutcome;
  if (length > opts_.max_chunk_bytes) {
    return Result<Outcome>::Failure(common::MakeError(
        ErrorCode::kBadRequest,
        "GDS GET chunk exceeds 1 GiB cuObjServer limit", false));
  }
  if (length == 0U) {
    return Result<Outcome>::Success(Outcome{"gds-cuobject-rdma-write-empty", 0});
  }

  auto server_lookup = GetServer();
  if (!server_lookup.success()) {
    return Result<Outcome>::Failure(server_lookup.error());
  }

  // mu_ 仅短暂拷贝 pool 引用；Acquire 在锁外执行避免成为热点。
  std::shared_ptr<PinnedBufferPool> pool_ref;
  {
    std::scoped_lock lock(mu_);
    pool_ref = buffer_pool_;
  }
  PinnedBufferLease lease;
  if (pool_ref) {
    lease = pool_ref->Acquire(static_cast<std::size_t>(length));
  }
  if (!lease.ok()) {
    return Result<Outcome>::Failure(common::MakeError(
        ErrorCode::kRdmaUnavailable, "cuObjServer pinned buffer alloc failed"));
  }

  // backend 读到 pinned buffer，再交给 cuObjServer RDMA 到 GPU。
  auto* staging = static_cast<std::byte*>(lease.data());
  auto read = backend_.Read(session.bucket, session.object_key, object_offset,
                            std::span<std::byte>(staging,
                                                  static_cast<std::size_t>(length)));
  if (!read.success()) {
    return Result<Outcome>::Failure(read.error());
  }
  if (read.value() == 0U) {
    return Result<Outcome>::Success(Outcome{"gds-cuobject-rdma-write-empty", 0});
  }
  const std::size_t actual_size = read.value();

  // 在 RDMA 推送之前算 CRC32C，反映 server 真正读到的字节；client 用同一
  // 字节序列在 GPU 端再算一次（或信任 server）做 end-to-end 校验。
  const std::uint32_t crc = common::Crc32c(
      std::span<const std::byte>(staging, actual_size));

  auto server = server_lookup.value();
  const auto channel = server->allocateChannelId();
  if (channel == INVALID_CHANNEL_ID) {
    return Result<Outcome>::Failure(common::MakeError(
        ErrorCode::kRdmaUnavailable, "cuObjServer allocateChannelId failed"));
  }
  ChannelGuard chan_guard(*server, channel);

  const auto remote_buf_start = ParseRemoteBufferAddress(rdma_token);
  ibv_wc_status status = IBV_WC_SUCCESS;
  const auto transferred = server->handleGetObject(
      BuildObjectId(session), lease.mr(), remote_buf_start, actual_size,
      rdma_token, channel, 0, &status, nullptr);
  if (logger_ != nullptr) {
    logger_->info("gds.get object={} offset={} length={} transferred={} status={} crc32c={:x}",
                  BuildObjectId(session), object_offset, length, transferred,
                  DescribeStatus(status), crc);
  }
  if (transferred < 0) {
    return Result<Outcome>::Failure(common::MakeError(
        ErrorCode::kRpcError,
        "cuObjServer handleGetObject failed: " + DescribeStatus(status)));
  }
  if (static_cast<std::size_t>(transferred) != actual_size) {
    return Result<Outcome>::Failure(common::MakeError(
        ErrorCode::kRpcError,
        "cuObjServer handleGetObject short transfer"));
  }
  return Result<Outcome>::Success(Outcome{"gds-cuobject-rdma-write", crc});
}

// 单对象 PUT：RDMA-READ from GPU → pinned buffer → backend.WriteRange。
// total_size = session.expected_size，触发 backend 预扩容。
Result<ObjectMetadata> GdsExecutor::PutChunk(const core::Session& session,
                                             const std::string& rdma_token,
                                             std::uint64_t object_offset,
                                             std::uint64_t length) {
  if (length > opts_.max_chunk_bytes) {
    return Result<ObjectMetadata>::Failure(common::MakeError(
        ErrorCode::kBadRequest,
        "GDS PUT chunk exceeds 1 GiB cuObjServer limit", false));
  }
  const std::optional<std::size_t> total_size =
      session.expected_size != 0U
          ? std::optional<std::size_t>(
                static_cast<std::size_t>(session.expected_size))
          : std::nullopt;
  // length=0 仅用于触发 size 扩容，不走 RDMA。
  if (length == 0U) {
    return backend_.WriteRange(session.bucket, session.object_key,
                               object_offset, {}, total_size);
  }

  auto server_lookup = GetServer();
  if (!server_lookup.success()) {
    return Result<ObjectMetadata>::Failure(server_lookup.error());
  }
  auto server = server_lookup.value();

  std::shared_ptr<PinnedBufferPool> pool_ref;
  {
    std::scoped_lock lock(mu_);
    pool_ref = buffer_pool_;
  }
  PinnedBufferLease lease;
  if (pool_ref) {
    lease = pool_ref->Acquire(static_cast<std::size_t>(length));
  }
  if (!lease.ok()) {
    return Result<ObjectMetadata>::Failure(common::MakeError(
        ErrorCode::kRdmaUnavailable, "cuObjServer pinned buffer alloc failed"));
  }

  const auto channel = server->allocateChannelId();
  if (channel == INVALID_CHANNEL_ID) {
    return Result<ObjectMetadata>::Failure(common::MakeError(
        ErrorCode::kRdmaUnavailable, "cuObjServer allocateChannelId failed"));
  }
  ChannelGuard chan_guard(*server, channel);

  const auto remote_buf_start = ParseRemoteBufferAddress(rdma_token);
  ibv_wc_status status = IBV_WC_SUCCESS;
  const auto transferred = server->handlePutObject(
      BuildObjectId(session), lease.mr(), remote_buf_start,
      static_cast<std::size_t>(length), rdma_token, channel, 0, &status, nullptr);
  if (logger_ != nullptr) {
    logger_->info("gds.put object={} offset={} length={} transferred={} status={}",
                  BuildObjectId(session), object_offset, length, transferred,
                  DescribeStatus(status));
  }
  if (transferred < 0) {
    return Result<ObjectMetadata>::Failure(common::MakeError(
        ErrorCode::kRpcError,
        "cuObjServer handlePutObject failed: " + DescribeStatus(status)));
  }

  const auto bytes = static_cast<std::size_t>(transferred);
  std::span<const std::byte> view(
      static_cast<const std::byte*>(lease.data()), bytes);
  auto write = backend_.WriteRange(session.bucket, session.object_key,
                                   object_offset, view, total_size);
  if (write.success() && logger_ != nullptr) {
    logger_->info("gds.put.persist object={} size={} etag={}",
                  BuildObjectId(session), write.value().content_length,
                  write.value().etag);
  }
  return write;
}

// Multipart PUT chunk：字节落到 backend.WritePart 而非 WriteRange。
// checksum_policy=="md5" 时跨 chunk 累积 MD5，返回 part 全量 MD5 etag。
Result<std::string> GdsExecutor::PutPart(const core::Session& session,
                                         const std::string& rdma_token,
                                         const std::string& upload_id,
                                         std::uint32_t part_number,
                                         std::uint64_t object_offset,
                                         std::uint64_t length,
                                         std::string_view checksum_policy,
                                         core::multipart::MultipartUpload* upload) {
  if (length > opts_.max_chunk_bytes) {
    return Result<std::string>::Failure(common::MakeError(
        ErrorCode::kBadRequest,
        "GDS PUT part exceeds 1 GiB cuObjServer limit", false));
  }
  if (length == 0U) {
    return backend_.WritePart(upload_id, part_number, object_offset,
                              std::span<const std::byte>{});
  }

  auto server_lookup = GetServer();
  if (!server_lookup.success()) {
    return Result<std::string>::Failure(server_lookup.error());
  }
  auto server = server_lookup.value();

  std::shared_ptr<PinnedBufferPool> pool_ref;
  {
    std::scoped_lock lock(mu_);
    pool_ref = buffer_pool_;
  }
  PinnedBufferLease lease;
  if (pool_ref) {
    lease = pool_ref->Acquire(static_cast<std::size_t>(length));
  }
  if (!lease.ok()) {
    return Result<std::string>::Failure(common::MakeError(
        ErrorCode::kRdmaUnavailable, "cuObjServer pinned buffer alloc failed"));
  }

  const auto channel = server->allocateChannelId();
  if (channel == INVALID_CHANNEL_ID) {
    return Result<std::string>::Failure(common::MakeError(
        ErrorCode::kRdmaUnavailable, "cuObjServer allocateChannelId failed"));
  }
  ChannelGuard chan_guard(*server, channel);

  const auto remote_buf_start = ParseRemoteBufferAddress(rdma_token);
  ibv_wc_status status = IBV_WC_SUCCESS;
  const auto transferred = server->handlePutObject(
      BuildObjectId(session), lease.mr(), remote_buf_start,
      static_cast<std::size_t>(length), rdma_token, channel, 0, &status, nullptr);
  if (logger_ != nullptr) {
    logger_->info(
        "gds.put_part upload_id={} part={} offset={} length={} transferred={} status={}",
        upload_id, part_number, object_offset, length, transferred,
        DescribeStatus(status));
  }
  if (transferred < 0) {
    return Result<std::string>::Failure(common::MakeError(
        ErrorCode::kRpcError,
        "cuObjServer handlePutObject failed: " + DescribeStatus(status)));
  }

  const auto bytes = static_cast<std::size_t>(transferred);
  std::span<const std::byte> view(
      static_cast<const std::byte*>(lease.data()), bytes);
  auto write = backend_.WritePart(upload_id, part_number, object_offset, view);
  if (!write.success()) {
    return write;
  }
  if (checksum_policy == "md5") {
    // 跨 chunk 累积 MD5：cuObj 可能将一个 part 切成多次 chunk RPC，
    // 需要跨 chunk 维护 MD5_CTX 才能得到正确的 part-level MD5 etag。
    if (upload != nullptr) {
      std::scoped_lock lock(upload->part_md5_mu);
      auto it = upload->part_md5_ctxs.find(part_number);
      if (it == upload->part_md5_ctxs.end()) {
        MD5_CTX ctx;
        MD5_Init(&ctx);
        MD5_Update(&ctx, lease.data(), bytes);
        upload->part_md5_ctxs.emplace(part_number, ctx);
      } else {
        MD5_Update(&it->second, lease.data(), bytes);
      }
      // 拷贝 CTX → finalize 拷贝得到运行中 MD5；原 CTX 保留给后续 chunk。
      MD5_CTX copy = upload->part_md5_ctxs.at(part_number);
      std::array<unsigned char, MD5_DIGEST_LENGTH> digest{};
      MD5_Final(digest.data(), &copy);
      std::string out;
      out.reserve(MD5_DIGEST_LENGTH * 2 + 2);
      out.push_back('"');
      char buf[3];
      for (auto b : digest) {
        std::snprintf(buf, sizeof(buf), "%02x", b);
        out.append(buf, 2);
      }
      out.push_back('"');
      return Result<std::string>::Success(std::move(out));
    }
    // fallback：无 upload 对象时退回单 chunk MD5（保持旧行为）
    return Result<std::string>::Success(Md5Hex(lease.data(), bytes));
  }
  return write;
}

}  // namespace us3_turbo_access::gateway::data_path::gds
