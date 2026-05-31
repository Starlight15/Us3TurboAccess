#include "data_path/rdma/rdma_executor.h"

#include <infiniband/verbs.h>
#include <sys/mman.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "common/crc32c.h"
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

// 解码 client 传来的 base64(big-endian u32) → uint32 CRC32C。
// 格式与 HTTP 路径的 x-amz-checksum-crc32c 一致；解析失败返回 nullopt。
[[nodiscard]] std::optional<std::uint32_t>
ParseClientCrc32c(std::string_view s) {
  if (s.empty()) return std::nullopt;
  auto idx_of = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<std::uint8_t> out;
  std::uint32_t buf = 0;
  int bits = 0;
  for (char c : s) {
    if (c == '=') break;
    int v = idx_of(c);
    if (v < 0) return std::nullopt;
    buf = (buf << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((buf >> bits) & 0xFFu));
    }
  }
  if (out.size() < 4) return std::nullopt;
  return (static_cast<std::uint32_t>(out[0]) << 24) |
         (static_cast<std::uint32_t>(out[1]) << 16) |
         (static_cast<std::uint32_t>(out[2]) << 8) |
         static_cast<std::uint32_t>(out[3]);
}

// 验证 client_crc32c 是否与 server 端 pinned buffer 实算 CRC 一致。
// client_crc_b64 为空表示 client 没发送（向后兼容），返回 success。
[[nodiscard]] Result<bool>
VerifyClientCrc32c(std::span<const std::byte> view,
                   std::string_view client_crc_b64) {
  if (client_crc_b64.empty()) {
    return Result<bool>::Success(true);  // client 未发，跳过校验
  }
  auto client_crc = ParseClientCrc32c(client_crc_b64);
  if (!client_crc) {
    return Result<bool>::Failure(common::MakeError(
        ErrorCode::kBadRequest,
        "invalid client_checksum format (expected base64 of big-endian uint32)"));
  }
  const auto server_crc = common::Crc32c(view);
  if (*client_crc != server_crc) {
    return Result<bool>::Failure(common::MakeError(
        ErrorCode::kInvalidArgument,
        "RDMA commit CRC32C mismatch: client=" +
            std::to_string(*client_crc) + " server=" +
            std::to_string(server_crc)));
  }
  return Result<bool>::Success(true);
}

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
  // EraseSessionsCb 把 set_on_release 的 vector callback 转给 session_registry_。
  struct EraseSessionsCb {
    RdmaSessionRegistry* registry;
    void operator()(const std::vector<std::string>& session_ids) const {
      if (registry == nullptr) return;
      for (const auto& sid : session_ids) {
        (void)registry->Erase(sid);
      }
    }
  };
  connection_registry_->set_on_release(
      EraseSessionsCb{session_registry_.get()});
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
  // SweeperAbortCb 把 sweeper 的过期回调路由到本 executor 的 AbortSession。
  struct SweeperAbortCb {
    RdmaExecutor* exec;
    void operator()(const std::string& session_id) const {
      (void)exec->AbortSession(session_id);
    }
  };
  if (opts_.session_ttl.count() > 0) {
    sweeper_ = std::make_unique<RdmaSessionSweeper>(
        *session_registry_, opts_.session_sweep_interval,
        SweeperAbortCb{this},
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

  // 失败时把 CAS 抢到的 kBinding 状态归位回 kUnbound，让下一次 Bind 可以再来。
  // 仅一行，沿着错误路径手动写出而不抽 lambda，便于回看时不会漏掉某条路径。

  const std::size_t cap = RoundUpPage(entry->expected_bytes);
  void* buf = std::aligned_alloc(kPageSize, cap);
  if (buf == nullptr) {
    entry->bind_state.store(RdmaSessionBindState::kUnbound,
                              std::memory_order_release);
    return Result<RdmaBindInfo>::Failure(MakeRdmaError("aligned_alloc failed"));
  }
  if (::mlock(buf, cap) != 0) {
    std::free(buf);
    entry->bind_state.store(RdmaSessionBindState::kUnbound,
                              std::memory_order_release);
    return Result<RdmaBindInfo>::Failure(MakeRdmaError(
        "mlock failed (check `ulimit -l`)"));
  }
  ibv_mr* mr = ibv_reg_mr(conn->pd, buf, cap,
                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (mr == nullptr) {
    ::munlock(buf, cap);
    std::free(buf);
    entry->bind_state.store(RdmaSessionBindState::kUnbound,
                              std::memory_order_release);
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
                                                    std::uint64_t bytes_transferred,
                                                    std::string_view client_crc32c_b64) {
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

  // 端到端 CRC 校验：client_crc32c_b64 非空时与 server 算的 CRC 比对，
  // 不一致拒绝 commit；buffer/session 会被 AbortSession 路径或 sweeper 清理。
  auto verify = VerifyClientCrc32c(view, client_crc32c_b64);
  if (!verify.success()) {
    return Result<RdmaCommitInfo>::Failure(verify.error());
  }

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
    std::uint32_t part_number, std::uint64_t bytes_transferred,
    std::string_view client_crc32c_b64) {
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

  // 端到端 CRC 校验（与 CommitObject 对称）。
  auto verify = VerifyClientCrc32c(view, client_crc32c_b64);
  if (!verify.success()) {
    return Result<RdmaCommitPartInfo>::Failure(verify.error());
  }

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
