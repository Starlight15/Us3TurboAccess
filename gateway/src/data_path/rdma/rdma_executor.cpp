#include "data_path/rdma/rdma_executor.h"

#include <infiniband/verbs.h>
#include <sys/mman.h>

#include <cstdint>
#include <cstdlib>
#include <utility>

#include "common/error.h"
#include "core/metadata/metadata_service.h"
#include "core/multipart/multipart_coordinator.h"
#include "data_path/rdma/rdma_connection_registry.h"
#include "data_path/rdma/rdma_listener.h"
#include "data_path/rdma/rdma_resources.h"
#include "data_path/rdma/rdma_session_registry.h"
#include "data_path/rdma/rdma_session_sweeper.h"
#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway::data_path::rdma {

namespace {

constexpr std::size_t kPageSize = 4096;

[[nodiscard]] Error MakeRdmaError(std::string message) {
  Error err;
  err.code = ErrorCode::kRdmaUnavailable;
  err.message = std::move(message);
  err.retryable = false;
  return err;
}

[[nodiscard]] std::size_t RoundUpPage(std::size_t n) {
  return (n + kPageSize - 1) / kPageSize * kPageSize;
}

}  // namespace

RdmaExecutor::RdmaExecutor(std::string public_host, std::string bind_host,
                            const RdmaOptions& opts, backend::IBackend& backend,
                            core::MetadataService& metadata,
                            core::multipart::MultipartCoordinator* multipart,
                            runtime::IoWorkerPool* io_pool,
                            std::shared_ptr<spdlog::logger> logger)
    : public_host_(std::move(public_host)),
      bind_host_(std::move(bind_host)),
      opts_(opts),
      backend_(backend),
      metadata_(metadata),
      multipart_(multipart),
      io_pool_(io_pool),
      logger_(std::move(logger)) {}

RdmaExecutor::~RdmaExecutor() { Stop(); }

std::string RdmaExecutor::endpoint() const {
  return public_host_ + ":" + std::to_string(opts_.listen_port);
}

bool RdmaExecutor::available() const {
  std::scoped_lock lock(mu_);
  return started_ && listener_ != nullptr && listener_->running();
}

Result<bool> RdmaExecutor::Start() {
  std::scoped_lock lock(mu_);
  if (started_) {
    return Result<bool>::Success(true);
  }
  auto res = RdmaResources::Open(opts_);
  if (!res.success()) {
    return Result<bool>::Failure(res.error());
  }
  std::unique_ptr<RdmaResources> uniq = std::move(res.value());
  resources_ = std::shared_ptr<RdmaResources>(std::move(uniq));

  session_registry_    = std::make_unique<RdmaSessionRegistry>();
  connection_registry_ = std::make_unique<RdmaConnectionRegistry>();
  // 连接销毁前先释放挂在它 PD 上的所有 session（dereg MR + free buffer），
  // 否则 ibv_dealloc_pd 会带走仍被 MR 引用的 PD，触发 UB。
  connection_registry_->set_on_release(
      [this](const std::vector<std::string>& session_ids) {
        if (session_registry_ == nullptr) return;
        for (const auto& sid : session_ids) {
          (void)session_registry_->Erase(sid);
        }
      });
  listener_ = std::make_unique<RdmaListener>(
      resources_, connection_registry_.get(), opts_, io_pool_, logger_);
  auto started = listener_->Start(bind_host_);
  if (!started.success()) {
    listener_.reset();
    session_registry_.reset();
    connection_registry_.reset();
    resources_.reset();
    return Result<bool>::Failure(started.error());
  }

  // TTL > 0 才起 sweeper；sweeper 在锁外通过 callback 调 AbortSession 路径，
  // 顺手做 backend 资源回收 + DetachSession（与 client AbortSession RPC 一致）。
  if (opts_.session_ttl.count() > 0) {
    sweeper_ = std::make_unique<RdmaSessionSweeper>(
        *session_registry_, opts_.session_sweep_interval,
        [this](const std::string& session_id) {
          (void)this->AbortSession(session_id);
        },
        logger_);
    sweeper_->Start();
  }

  started_ = true;
  if (logger_ != nullptr) {
    logger_->info("rdma: executor ready device={} gid={} listen={}:{}",
                  resources_->device_name(), resources_->gid_string(),
                  bind_host_, opts_.listen_port);
  }
  return Result<bool>::Success(true);
}

void RdmaExecutor::Stop() {
  std::unique_ptr<RdmaListener>           listener_to_destroy;
  std::unique_ptr<RdmaSessionSweeper>     sweeper_to_destroy;
  std::unique_ptr<RdmaSessionRegistry>    session_to_destroy;
  std::unique_ptr<RdmaConnectionRegistry> connection_to_destroy;
  std::shared_ptr<RdmaResources>          resources_to_release;
  {
    std::scoped_lock lock(mu_);
    if (!started_) return;
    started_ = false;
    listener_to_destroy    = std::move(listener_);
    sweeper_to_destroy     = std::move(sweeper_);
    session_to_destroy     = std::move(session_registry_);
    connection_to_destroy  = std::move(connection_registry_);
    resources_to_release   = std::move(resources_);
  }
  // 顺序：
  //   1) listener：停 event loop，断新连接进入。
  //   2) sweeper：停后台线程，避免它在我们拆下层时还在调 callback。
  //   3) connection_registry：析构时通过 on_release 反向调 session_registry
  //      释放挂在每条连接上的 MR/buffer（必须在 ibv_dealloc_pd 之前完成），
  //      所以 connection_to_destroy 必须先于 session_to_destroy 析构，并且
  //      析构期间 session_to_destroy 还要存活。
  //   4) session_registry：兜底释放未绑定连接的孤儿 session（仅有 metadata）。
  //   5) resources：仅含 device + GID 缓存，最后释放。
  listener_to_destroy.reset();
  sweeper_to_destroy.reset();
  connection_to_destroy.reset();
  session_to_destroy.reset();
  resources_to_release.reset();
}

Result<bool> RdmaExecutor::OnSessionOpened(const core::Session& session) {
  if (!available()) {
    return Result<bool>::Failure(MakeRdmaError("rdma executor not available"));
  }
  if (session.op != OperationType::kPut || session.expected_size == 0U) {
    return Result<bool>::Success(true);
  }

  // multipart 单 part：整对象空间在 StartUpload 时已 Reserve，这里只登记 session；
  // expected_size 解释为本 part 字节数，用于 BindSession 阶段分配 buffer。
  if (!session.is_multipart_part) {
    auto reserved = metadata_.Reserve(
        session.bucket, session.object_key,
        static_cast<std::size_t>(session.expected_size));
    if (!reserved.success()) {
      return Result<bool>::Failure(reserved.error());
    }
  }

  std::scoped_lock lock(mu_);
  if (session_registry_ == nullptr) {
    return Result<bool>::Failure(MakeRdmaError("rdma executor not ready"));
  }
  session_registry_->CreateForSession(
      session.session_id, session.bucket, session.object_key,
      static_cast<std::size_t>(session.expected_size),
      std::chrono::duration_cast<std::chrono::milliseconds>(opts_.session_ttl));
  return Result<bool>::Success(true);
}

Result<RdmaDiscoverInfo> RdmaExecutor::DiscoverEndpoint(
    std::string_view session_id) {
  auto entry = session_registry_ != nullptr
                   ? session_registry_->Find(session_id)
                   : nullptr;
  if (entry == nullptr) {
    return Result<RdmaDiscoverInfo>::Failure(MakeRdmaError(
        "rdma session not found at discover"));
  }
  RdmaDiscoverInfo info;
  info.host          = public_host_;
  info.port          = static_cast<std::uint32_t>(opts_.listen_port);
  info.max_msg_bytes = static_cast<std::uint64_t>(opts_.max_msg_bytes);
  return Result<RdmaDiscoverInfo>::Success(std::move(info));
}

/*
 * 在指定 connection 的 PD 上为本 session 分配接收 buffer + 注册 MR。
 * 失败时已申请的资源按逆序回收（buffer mlock → buffer → MR）。
 * 同一 session 重复 Bind：拒绝（防资源重复申请）。
 */
Result<RdmaBindInfo> RdmaExecutor::BindSessionToConnection(
    std::string_view session_id, std::uint64_t conn_token) {
  auto entry = session_registry_ != nullptr
                   ? session_registry_->Find(session_id)
                   : nullptr;
  if (entry == nullptr) {
    return Result<RdmaBindInfo>::Failure(MakeRdmaError(
        "rdma session not found at bind"));
  }
  if (entry->expected_bytes == 0) {
    return Result<RdmaBindInfo>::Failure(MakeRdmaError(
        "rdma session has zero expected_bytes at bind"));
  }
  auto conn = connection_registry_ != nullptr
                  ? connection_registry_->Find(conn_token)
                  : nullptr;
  if (conn == nullptr) {
    return Result<RdmaBindInfo>::Failure(MakeRdmaError(
        "rdma conn_token not found (connection closed?)"));
  }

  // CAS 抢绑定权：只有第一个把 kUnbound → kBinding 的请求会继续；
  // 重复 Bind / 已 Bound / 正在 Binding 都走拒绝路径。
  RdmaSessionBindState expected = RdmaSessionBindState::kUnbound;
  if (!entry->bind_state.compare_exchange_strong(
          expected, RdmaSessionBindState::kBinding,
          std::memory_order_acq_rel)) {
    return Result<RdmaBindInfo>::Failure(MakeRdmaError(
        expected == RdmaSessionBindState::kBound
            ? "rdma session already bound to a connection"
            : "rdma session bind already in progress"));
  }

  // 失败回滚 helper：CAS 状态归位 + 释放半成品资源。
  auto rollback = [&]() {
    entry->bind_state.store(RdmaSessionBindState::kUnbound,
                              std::memory_order_release);
  };

  const std::size_t cap = RoundUpPage(entry->expected_bytes);
  void* buf = std::aligned_alloc(kPageSize, cap);
  if (buf == nullptr) {
    rollback();
    return Result<RdmaBindInfo>::Failure(MakeRdmaError("aligned_alloc failed"));
  }
  if (::mlock(buf, cap) != 0) {
    std::free(buf);
    rollback();
    return Result<RdmaBindInfo>::Failure(MakeRdmaError(
        "mlock failed (ulimit -l 不足?)"));
  }
  ibv_mr* mr = ibv_reg_mr(conn->pd, buf, cap,
                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (mr == nullptr) {
    ::munlock(buf, cap);
    std::free(buf);
    rollback();
    return Result<RdmaBindInfo>::Failure(MakeRdmaError("ibv_reg_mr failed"));
  }

  entry->conn_token       = conn_token;
  entry->mr               = mr;
  entry->buffer_data      = buf;
  entry->buffer_capacity  = cap;
  entry->raddr            = reinterpret_cast<std::uint64_t>(buf);
  entry->rkey             = mr->rkey;
  entry->bind_state.store(RdmaSessionBindState::kBound,
                           std::memory_order_release);

  // 把 session 挂回连接，DISCONNECTED 时 connection_registry 会反向回调释放。
  connection_registry_->AttachSession(conn_token, std::string(session_id));

  RdmaBindInfo info;
  info.raddr = entry->raddr;
  info.rkey  = entry->rkey;
  return Result<RdmaBindInfo>::Success(std::move(info));
}

Result<RdmaCommitInfo> RdmaExecutor::CommitObject(std::string_view session_id,
                                                    std::uint64_t bytes_transferred) {
  auto entry = session_registry_ != nullptr
                   ? session_registry_->Find(session_id)
                   : nullptr;
  if (entry == nullptr) {
    return Result<RdmaCommitInfo>::Failure(MakeRdmaError(
        "rdma session not found at commit"));
  }
  if (entry->buffer_data == nullptr) {
    return Result<RdmaCommitInfo>::Failure(MakeRdmaError(
        "rdma buffer not bound at commit"));
  }
  if (bytes_transferred == 0 || bytes_transferred > entry->buffer_capacity) {
    return Result<RdmaCommitInfo>::Failure(common::MakeError(
        ErrorCode::kBadRequest,
        "commit bytes invalid (0 or beyond reserved buffer size)"));
  }

  std::span<const std::byte> view(
      static_cast<const std::byte*>(entry->buffer_data),
      static_cast<std::size_t>(bytes_transferred));
  auto write = backend_.WriteRange(
      entry->bucket, entry->object_key, /*offset=*/0, view,
      static_cast<std::size_t>(bytes_transferred));
  if (!write.success()) {
    return Result<RdmaCommitInfo>::Failure(write.error());
  }
  RdmaCommitInfo info;
  info.etag    = write.value().etag;
  info.version = write.value().version;
  // 释放 buffer + MR；连接保留以备复用。Erase 返回 conn_token，反向通知
  // ConnectionRegistry 把本 session 从该连接的 sessions 集合摘掉。
  const auto token = session_registry_->Erase(session_id);
  if (token != 0 && connection_registry_ != nullptr) {
    connection_registry_->DetachSession(token, std::string(session_id));
  }
  return Result<RdmaCommitInfo>::Success(std::move(info));
}

Result<bool> RdmaExecutor::AbortSession(std::string_view session_id) {
  if (session_registry_ == nullptr) {
    return Result<bool>::Success(false);
  }
  const auto token = session_registry_->Erase(session_id);
  if (token != 0 && connection_registry_ != nullptr) {
    connection_registry_->DetachSession(token, std::string(session_id));
  }
  if (logger_ != nullptr && token != 0) {
    logger_->info("rdma: aborted session={}", session_id);
  }
  return Result<bool>::Success(token != 0);
}

Result<RdmaCommitPartInfo> RdmaExecutor::CommitPart(
    std::string_view session_id, std::string_view upload_id,
    std::uint32_t part_number, std::uint64_t bytes_transferred) {
  auto entry = session_registry_ != nullptr
                   ? session_registry_->Find(session_id)
                   : nullptr;
  if (entry == nullptr) {
    return Result<RdmaCommitPartInfo>::Failure(MakeRdmaError(
        "rdma session not found at commit-part"));
  }
  if (entry->buffer_data == nullptr) {
    return Result<RdmaCommitPartInfo>::Failure(MakeRdmaError(
        "rdma buffer not bound at commit-part"));
  }
  if (bytes_transferred == 0 || bytes_transferred > entry->buffer_capacity) {
    return Result<RdmaCommitPartInfo>::Failure(common::MakeError(
        ErrorCode::kBadRequest,
        "commit-part bytes invalid"));
  }
  // upload_id 是 client 拿到的公开 id；backend.WritePart 要的是 backend_upload_id。
  // 与 GDS 路径对称（control_plane_service 在 GdsChunk 里做同样的查找）。
  if (multipart_ == nullptr) {
    return Result<RdmaCommitPartInfo>::Failure(MakeRdmaError(
        "multipart coordinator not configured at commit-part"));
  }
  auto lookup = multipart_->Lookup(upload_id);
  if (!lookup.success()) {
    return Result<RdmaCommitPartInfo>::Failure(lookup.error());
  }
  auto upload = lookup.value();

  std::span<const std::byte> view(
      static_cast<const std::byte*>(entry->buffer_data),
      static_cast<std::size_t>(bytes_transferred));
  auto part = backend_.WritePart(upload->backend_upload_id, part_number,
                                  /*offset=*/0, view);
  if (!part.success()) {
    return Result<RdmaCommitPartInfo>::Failure(part.error());
  }

  // 写完后登记 part 进度（part 序号 + 字节数 + etag），供 Complete 校验。
  multipart_->RegisterPart(*upload, part_number,
                            /*offset=*/0,
                            static_cast<std::uint64_t>(bytes_transferred),
                            part.value());

  RdmaCommitPartInfo info;
  info.part_etag = part.value();
  const auto token = session_registry_->Erase(session_id);
  if (token != 0 && connection_registry_ != nullptr) {
    connection_registry_->DetachSession(token, std::string(session_id));
  }
  return Result<RdmaCommitPartInfo>::Success(std::move(info));
}

}  // namespace us3_turbo_access::gateway::data_path::rdma
