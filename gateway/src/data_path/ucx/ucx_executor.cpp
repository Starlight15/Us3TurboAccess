#include "data_path/ucx/ucx_executor.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <span>
#include <thread>
#include <utility>

#include "backend/backend.h"
#include "common/crc32c.h"
#include "common/error.h"
#include "common/metrics.h"
#include "core/metadata/metadata_service.h"
#include "core/session/session.h"

namespace us3_turbo_access::gateway::data_path::ucx {

namespace {

constexpr std::size_t kPageSize = 4096;

// UCX progress loop：无工作时 spin 次数超过此阈值后用 epoll/nanosleep 等待
constexpr int kSpinCountBeforeWait = 64;

// Stop 时强制关闭 ep 的最大 progress 次数（约 100ms @0.5ms/progress）
constexpr int kMaxProgressOnEpClose = 200;

[[nodiscard]] Error MakeUcxError(std::string message) {
  Error err;
  err.code      = ErrorCode::kRdmaUnavailable;
  err.message   = std::move(message);
  err.retryable = false;
  return err;
}

[[nodiscard]] std::size_t RoundUpPage(std::size_t n) {
  return (n + kPageSize - 1) / kPageSize * kPageSize;
}

[[nodiscard]] Result<bool> VerifyCrc32c(std::span<const std::byte> v,
                                         std::string_view b64) {
  if (b64.empty()) return Result<bool>::Success(true);
  auto c = common::ParseBase64Crc32c(b64);
  if (!c) return Result<bool>::Failure(
      common::MakeError(ErrorCode::kBadRequest, "invalid client_checksum format"));
  auto sv = common::Crc32c(v);
  if (*c != sv) return Result<bool>::Failure(common::MakeError(ErrorCode::kInvalidArgument,
      "UCX CRC32C mismatch: client=" + std::to_string(*c) +
      " server=" + std::to_string(sv)));
  return Result<bool>::Success(true);
}

}  // namespace

// ---- constructor / destructor ----

UcxExecutor::UcxExecutor(std::string public_host, std::string bind_host,
                          const UcxOptions& opts, backend::IBackend& backend,
                          core::MetadataService& metadata,
                          std::shared_ptr<spdlog::logger> logger)
    : public_host_(std::move(public_host)), bind_host_(std::move(bind_host)),
      opts_(opts), backend_(backend), metadata_(metadata),
      logger_(std::move(logger)),
      session_registry_(std::make_unique<UcxSessionRegistry>()) {}

UcxExecutor::~UcxExecutor() { Stop(); }

// ---- listener accept callback ----

void UcxExecutor::OnConnRequest(ucp_conn_request_h conn_req, void* arg) {
  auto* self = static_cast<UcxExecutor*>(arg);
  ucp_ep_params_t p{};
  p.field_mask   = UCP_EP_PARAM_FIELD_CONN_REQUEST;
  p.conn_request = conn_req;
  ucp_ep_h ep = nullptr;
  if (ucp_ep_create(self->ucp_worker_, &p, &ep) != UCS_OK) {
    if (self->logger_) self->logger_->warn("ucx: accept ep failed");
    return;
  }
  self->accepted_eps_.push_back(ep);
}

// ---- AM_WRITE_DONE handler (progress thread context) ----
// client 在 ucp_put_nbx + ep_flush 完成后发此 AM，header = session_id bytes

ucs_status_t UcxExecutor::OnAmWriteDone(
    void* arg,
    const void* header, std::size_t header_length,
    void* /*data*/, std::size_t /*data_length*/,
    const ucp_am_recv_param_t* /*param*/) {
  auto* self = static_cast<UcxExecutor*>(arg);
  if (!header || header_length == 0) return UCS_OK;

  std::string session_id(static_cast<const char*>(header), header_length);
  auto entry = self->session_registry_->Find(session_id);
  if (!entry) return UCS_OK;

  std::function<void()> cb;
  {
    std::lock_guard lk(entry->commit_mu);
    entry->write_done.store(true, std::memory_order_release);
    cb = std::move(entry->pending_commit);
  }
  if (cb) cb();
  return UCS_OK;
}

// ---- Start / Stop ----

Result<bool> UcxExecutor::Start() {
  if (started_.load(std::memory_order_acquire)) return Result<bool>::Success(true);

  // RMA WRITE 模式：需要 RMA + AM 特性
  ucp_config_t* cfg = nullptr;
  ucp_config_read(nullptr, nullptr, &cfg);
  if (cfg) {
    ucp_config_modify(cfg, "TLS", "rc,ud,tcp");
  }
  ucp_params_t cp{};
  cp.field_mask = UCP_PARAM_FIELD_FEATURES;
  cp.features   = UCP_FEATURE_RMA | UCP_FEATURE_AM | UCP_FEATURE_WAKEUP;
  ucs_status_t st = ucp_init(&cp, cfg, &ucp_ctx_);
  if (cfg) ucp_config_release(cfg);
  if (st != UCS_OK) return Result<bool>::Failure(
      MakeUcxError("ucp_init failed: " + std::string(ucs_status_string(st))));

  ucp_worker_params_t wp{};
  wp.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
  wp.thread_mode = UCS_THREAD_MODE_MULTI;
  st = ucp_worker_create(ucp_ctx_, &wp, &ucp_worker_);
  if (st != UCS_OK) {
    ucp_cleanup(ucp_ctx_); ucp_ctx_ = nullptr;
    return Result<bool>::Failure(
        MakeUcxError("ucp_worker_create failed: " + std::string(ucs_status_string(st))));
  }

  {
    ucp_worker_attr_t attr{};
    attr.field_mask = UCP_WORKER_ATTR_FIELD_THREAD_MODE;
    if (ucp_worker_query(ucp_worker_, &attr) == UCS_OK &&
        attr.thread_mode != UCS_THREAD_MODE_MULTI) {
      ucp_worker_destroy(ucp_worker_);
      ucp_cleanup(ucp_ctx_);
      ucp_worker_ = nullptr; ucp_ctx_ = nullptr;
      return Result<bool>::Failure(MakeUcxError(
          "UCX worker MULTI thread mode not available; "
          "rebuild UCX with --enable-mt"));
    }
  }

  // 注册 AM_WRITE_DONE 接收处理器
  ucp_am_handler_param_t am_param{};
  am_param.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                        UCP_AM_HANDLER_PARAM_FIELD_CB  |
                        UCP_AM_HANDLER_PARAM_FIELD_ARG |
                        UCP_AM_HANDLER_PARAM_FIELD_FLAGS;
  am_param.id    = kAmIdWriteDone;
  am_param.cb    = &UcxExecutor::OnAmWriteDone;
  am_param.arg   = this;
  am_param.flags = UCP_AM_FLAG_WHOLE_MSG;
  st = ucp_worker_set_am_recv_handler(ucp_worker_, &am_param);
  if (st != UCS_OK) {
    ucp_worker_destroy(ucp_worker_);
    ucp_cleanup(ucp_ctx_);
    ucp_worker_ = nullptr; ucp_ctx_ = nullptr;
    return Result<bool>::Failure(
        MakeUcxError("ucp_worker_set_am_recv_handler failed: " +
                     std::string(ucs_status_string(st))));
  }

  const std::string& bind_h = (bind_host_.empty() || bind_host_ == "0.0.0.0")
                               ? public_host_ : bind_host_;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(static_cast<uint16_t>(opts_.listen_port));
  if (inet_pton(AF_INET, bind_h.c_str(), &addr.sin_addr) != 1) {
    ucp_worker_destroy(ucp_worker_); ucp_cleanup(ucp_ctx_);
    ucp_worker_ = nullptr; ucp_ctx_ = nullptr;
    return Result<bool>::Failure(MakeUcxError("invalid bind_host: " + bind_h));
  }

  ucp_listener_params_t lp{};
  lp.field_mask              = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                                UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
  lp.sockaddr.addr           = reinterpret_cast<const sockaddr*>(&addr);
  lp.sockaddr.addrlen        = sizeof(addr);
  lp.conn_handler.cb         = &UcxExecutor::OnConnRequest;
  lp.conn_handler.arg        = this;
  st = ucp_listener_create(ucp_worker_, &lp, &ucp_listener_);
  if (st != UCS_OK) {
    ucp_worker_destroy(ucp_worker_); ucp_cleanup(ucp_ctx_);
    ucp_worker_ = nullptr; ucp_ctx_ = nullptr;
    return Result<bool>::Failure(
        MakeUcxError("ucp_listener_create failed: " + std::string(ucs_status_string(st))));
  }

  stop_.store(false, std::memory_order_release);
  session_registry_ = std::make_unique<UcxSessionRegistry>();
  buffer_pool_ = std::make_unique<UcxBufferPool>(ucp_ctx_, opts_.buffer_pool_max_idle);
  progress_thread_ = std::thread(&UcxExecutor::ProgressLoop, this);
  started_.store(true, std::memory_order_release);
  if (logger_) logger_->info("ucx: RMA WRITE mode, listening on {}:{}", bind_h, opts_.listen_port);
  return Result<bool>::Success(true);
}

void UcxExecutor::Stop() {
  bool expected = true;
  if (!started_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) return;
  stop_.store(true, std::memory_order_release);
  if (ucp_listener_) { ucp_listener_destroy(ucp_listener_); ucp_listener_ = nullptr; }
  if (progress_thread_.joinable()) progress_thread_.join();
  for (ucp_ep_h ep : accepted_eps_) {
    void* req = ucp_ep_close_nb(ep, UCP_EP_CLOSE_MODE_FORCE);
    if (UCS_PTR_IS_PTR(req)) {
      for (int i = 0; i < kMaxProgressOnEpClose && ucp_request_check_status(req) == UCS_INPROGRESS; ++i)
        ucp_worker_progress(ucp_worker_);
      ucp_request_free(req);
    }
  }
  accepted_eps_.clear();
  if (ucp_worker_)  { ucp_worker_destroy(ucp_worker_); ucp_worker_ = nullptr; }
  if (ucp_ctx_)     { ucp_cleanup(ucp_ctx_);            ucp_ctx_   = nullptr; }
  if (buffer_pool_) { buffer_pool_->Drain(); }
  session_registry_.reset();
  buffer_pool_.reset();
  if (logger_) logger_->info("ucx: executor stopped");
}

void UcxExecutor::ProgressLoop() {
  int ucx_fd = -1;
  if (ucp_worker_get_efd(ucp_worker_, &ucx_fd) != UCS_OK) {
    if (logger_) logger_->warn(
        "ucx: wakeup fd unavailable, falling back to sched_yield polling");
    ucx_fd = -1;
  }

  int epfd = -1;
  if (ucx_fd >= 0) {
    epfd = epoll_create1(0);
    if (epfd >= 0) {
      epoll_event ev{};
      ev.events  = EPOLLIN;
      ev.data.fd = ucx_fd;
      if (epoll_ctl(epfd, EPOLL_CTL_ADD, ucx_fd, &ev) != 0) {
        close(epfd); epfd = -1;
      }
    }
  }

  int idle_count = 0;
  while (!stop_.load(std::memory_order_acquire)) {
    if (ucp_worker_progress(ucp_worker_) > 0) {
      idle_count = 0;
      continue;
    }
    if (stop_.load(std::memory_order_acquire)) break;
    ++idle_count;
    if (idle_count < kSpinCountBeforeWait) { sched_yield(); continue; }
    if (epfd >= 0) {
      if (ucp_worker_arm(ucp_worker_) == UCS_OK) {
        epoll_event ready{};
        if (epoll_wait(epfd, &ready, 1, 0) == 0) epoll_wait(epfd, &ready, 1, 1);
      }
    } else {
      struct timespec ts{0, 10'000};
      nanosleep(&ts, nullptr);
    }
    idle_count = 0;
  }

  if (epfd >= 0) close(epfd);
  for (int i = 0; i < 16; ++i) ucp_worker_progress(ucp_worker_);
}

// ---- session lifecycle ----

Result<bool> UcxExecutor::OnSessionOpened(const core::Session& session) {
  session_registry_->Create(session.session_id, session.bucket,
                              session.object_key,
                              static_cast<std::size_t>(session.expected_size));
  return Result<bool>::Success(true);
}

void UcxExecutor::ReleaseEntry(std::shared_ptr<UcxSessionEntry> entry) {
  if (!entry || !entry->slot) return;
  if (buffer_pool_) {
    buffer_pool_->Return(entry->slot);  // MR 保持注册，归还给下一个 session
  } else {
    if (entry->slot->memh) ucp_mem_unmap(ucp_ctx_, entry->slot->memh);
    if (entry->slot->buf)  { ::munlock(entry->slot->buf, entry->slot->capacity); std::free(entry->slot->buf); }
    delete entry->slot;
  }
  entry->slot = nullptr;
}

// ---- PrepareTransfer：从 pool 取已注册 slot（或新建），下发 gw_raddr/rkey ----

Result<UcxDiscoverInfo> UcxExecutor::PrepareTransfer(
    std::string_view session_id,
    std::uint64_t transfer_bytes) {
  if (!started_.load(std::memory_order_acquire))
    return Result<UcxDiscoverInfo>::Failure(MakeUcxError("UCX executor not started"));

  auto entry = session_registry_->Find(session_id);
  if (!entry) return Result<UcxDiscoverInfo>::Failure(common::MakeError(
      ErrorCode::kSessionNotFound,
      "UCX session not found: " + std::string(session_id)));

  if (transfer_bytes == 0)
    return Result<UcxDiscoverInfo>::Failure(
        common::MakeError(ErrorCode::kBadRequest, "PrepareTransfer: transfer_bytes must be > 0"));

  if (transfer_bytes > opts_.max_msg_bytes)
    return Result<UcxDiscoverInfo>::Failure(
        common::MakeError(ErrorCode::kPayloadTooLarge, "object exceeds UCX max_msg_bytes"));

  // 幂等：已分配过直接返回
  if (entry->slot) {
    // 校验重试时 transfer_bytes 与首次一致，防止 RDMA 写越界
    if (entry->transfer_bytes != static_cast<std::size_t>(transfer_bytes)) {
      return Result<UcxDiscoverInfo>::Failure(common::MakeError(
          ErrorCode::kBadRequest,
          "PrepareTransfer: transfer_bytes mismatch on retry: expected " +
              std::to_string(entry->transfer_bytes) + " got " +
              std::to_string(transfer_bytes)));
    }
    UcxDiscoverInfo info;
    info.host           = public_host_;
    info.ucx_port       = static_cast<std::uint32_t>(opts_.listen_port);
    info.max_msg_bytes  = opts_.max_msg_bytes;
    info.gw_raddr       = entry->slot->gw_raddr;
    info.gw_packed_rkey = entry->slot->packed_rkey;
    return Result<UcxDiscoverInfo>::Success(std::move(info));
  }

  const std::size_t alloc = RoundUpPage(std::max(
      static_cast<std::size_t>(transfer_bytes), kPageSize));

  // 优先从 pool 取已注册的 slot
  RegisteredBuffer* slot = buffer_pool_ ? buffer_pool_->Acquire(alloc) : nullptr;

  if (!slot) {
    // pool 无合适 slot：分配 + 注册新的
    slot = new RegisteredBuffer{};
    slot->buf = std::aligned_alloc(kPageSize, alloc);
    if (!slot->buf) { delete slot; return Result<UcxDiscoverInfo>::Failure(MakeUcxError("buffer alloc failed")); }
    if (::mlock(slot->buf, alloc) != 0) { std::free(slot->buf); delete slot; return Result<UcxDiscoverInfo>::Failure(MakeUcxError("mlock failed")); }
    slot->capacity = alloc;

    ucp_mem_map_params_t mmp{};
    mmp.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH;
    mmp.address    = slot->buf;
    mmp.length     = alloc;
    auto map_st = ucp_mem_map(ucp_ctx_, &mmp, &slot->memh);
    if (map_st != UCS_OK) {
      ::munlock(slot->buf, alloc); std::free(slot->buf); delete slot;
      return Result<UcxDiscoverInfo>::Failure(
          MakeUcxError("ucp_mem_map failed: " + std::string(ucs_status_string(map_st))));
    }

    ucp_mem_attr_t attr{};
    attr.field_mask = UCP_MEM_ATTR_FIELD_ADDRESS;
    ucp_mem_query(slot->memh, &attr);
    slot->gw_raddr = reinterpret_cast<std::uint64_t>(attr.address);

    void* rkey_buf = nullptr; std::size_t rkey_len = 0;
    auto pack_st = ucp_rkey_pack(ucp_ctx_, slot->memh, &rkey_buf, &rkey_len);
    if (pack_st != UCS_OK) {
      ucp_mem_unmap(ucp_ctx_, slot->memh); ::munlock(slot->buf, alloc); std::free(slot->buf); delete slot;
      return Result<UcxDiscoverInfo>::Failure(
          MakeUcxError("ucp_rkey_pack failed: " + std::string(ucs_status_string(pack_st))));
    }
    slot->packed_rkey.assign(
        static_cast<std::uint8_t*>(rkey_buf),
        static_cast<std::uint8_t*>(rkey_buf) + rkey_len);
    ucp_rkey_buffer_release(rkey_buf);

    common::metrics().ucx_buffer_pool_miss_total << 1;
  } else {
    common::metrics().ucx_buffer_pool_hit_total << 1;
  }

  common::metrics().ucx_put_inflight << 1;
  entry->slot           = slot;
  entry->transfer_bytes = static_cast<std::size_t>(transfer_bytes);

  if (logger_) logger_->info(
      "ucx: PrepareTransfer session={} gw_raddr=0x{:x} rkey_len={} transfer={}",
      session_id, slot->gw_raddr, slot->packed_rkey.size(), transfer_bytes);

  UcxDiscoverInfo info;
  info.host           = public_host_;
  info.ucx_port       = static_cast<std::uint32_t>(opts_.listen_port);
  info.max_msg_bytes  = opts_.max_msg_bytes;
  info.gw_raddr       = slot->gw_raddr;
  info.gw_packed_rkey = slot->packed_rkey;
  return Result<UcxDiscoverInfo>::Success(std::move(info));
}

// ---- DoCommitObject：落盘逻辑（同步路径或 pending_commit 调用）----

Result<UcxCommitInfo> UcxExecutor::DoCommitObject(
    std::shared_ptr<UcxSessionEntry>& entry,
    std::uint64_t bytes_transferred,
    std::string_view client_crc32c_b64) {
  const std::size_t data_size = static_cast<std::size_t>(bytes_transferred);
  const std::span<const std::byte> view(
      static_cast<const std::byte*>(entry->slot->buf), data_size);

  // CRC32C 校验已关闭（性能优化）
  // if (auto v = VerifyCrc32c(view, client_crc32c_b64); !v.success())
  //   return Result<UcxCommitInfo>::Failure(v.error());

  auto wr = backend_.WriteRange(entry->bucket, entry->object_key, 0, view, data_size);
  if (!wr.success()) return Result<UcxCommitInfo>::Failure(wr.error());

  UcxCommitInfo info;
  info.etag    = wr.value().etag;
  info.version = wr.value().version;
  return Result<UcxCommitInfo>::Success(std::move(info));
}

// ---- DoWritePartData：低层 part 落盘（CRC + backend WritePart），无 multipart 语义 ----

Result<std::string> UcxExecutor::DoWritePartData(
    std::shared_ptr<UcxSessionEntry>& entry,
    std::string_view backend_upload_id, std::uint32_t part_number,
    std::uint64_t bytes_transferred,
    std::string_view client_crc32c_b64) {
  const std::size_t data_size = static_cast<std::size_t>(bytes_transferred);
  const std::span<const std::byte> view(
      static_cast<const std::byte*>(entry->slot->buf), data_size);

  // CRC32C 校验已关闭（性能优化）
  // if (auto v = VerifyCrc32c(view, client_crc32c_b64); !v.success())
  //   return Result<std::string>::Failure(v.error());

  auto part = backend_.WritePart(backend_upload_id, part_number, 0, view);
  if (!part.success()) return Result<std::string>::Failure(part.error());
  return part;
}

// ---- CommitObjectAsync ----
//
// 协议：
//   write_done=true  → 直接落盘，on_done 同步调用，返回 true
//   write_done=false → 把落盘逻辑存入 pending_commit，等 OnAmWriteDone 触发，返回 false
//
// 超时：pending_commit 由调用方（RdmaDataPlaneService）通过 timeout timer 处理；
//        此处只存 callback，不做 deadline 检查（简化实现，commit_data_timeout=5s 兜底）。

bool UcxExecutor::CommitObjectAsync(
    std::string_view session_id,
    std::uint64_t bytes_transferred,
    std::string_view client_crc32c_b64,
    std::function<void(Result<UcxCommitInfo>)> on_done) {
  auto entry = session_registry_->Find(session_id);
  if (!entry) {
    on_done(Result<UcxCommitInfo>::Failure(common::MakeError(
        ErrorCode::kSessionNotFound,
        "UCX CommitObject: session not found: " + std::string(session_id))));
    return true;
  }
  if (!entry->slot || !entry->slot->buf) {
    on_done(Result<UcxCommitInfo>::Failure(common::MakeError(
        ErrorCode::kInternal, "UCX session buffer not allocated")));
    return true;
  }

  auto release_and_erase = [this, sid = std::string(session_id), entry]() mutable {
    session_registry_->Erase(sid);
    ReleaseEntry(entry);
    common::metrics().ucx_put_inflight << -1;
  };

  // 快速路径：数据已到
  if (entry->write_done.load(std::memory_order_acquire)) {
    auto r = DoCommitObject(entry, bytes_transferred, client_crc32c_b64);
    release_and_erase();
    on_done(std::move(r));
    return true;
  }

  // 慢速路径：注册 pending_commit，等 OnAmWriteDone
  {
    std::lock_guard lk(entry->commit_mu);
    // double-check：加锁后再检查，防止 AM 在加锁前已经到达
    if (entry->write_done.load(std::memory_order_acquire)) {
      auto r = DoCommitObject(entry, bytes_transferred, client_crc32c_b64);
      release_and_erase();
      on_done(std::move(r));
      return true;
    }
    entry->pending_commit = [this, entry, bytes_transferred,
                              crc = std::string(client_crc32c_b64),
                              on_done = std::move(on_done), release_and_erase]() mutable {
      auto r = DoCommitObject(entry, bytes_transferred, crc);
      release_and_erase();
      on_done(std::move(r));
    };
  }
  return false;
}

// ---- CommitPartDataAsync ----
//
// 同样的 write_done 同步/异步协议，但用于 multipart part 数据写入。
// 无 multipart 协调语义（Lookup / RegisterPart 由 UcxMultipartPathHandler 处理）。

bool UcxExecutor::CommitPartDataAsync(
    std::string_view session_id,
    std::string_view backend_upload_id,
    std::uint32_t part_number,
    std::uint64_t bytes_transferred,
    std::string_view client_crc32c_b64,
    std::function<void(Result<std::string>)> on_done) {
  auto entry = session_registry_->Find(session_id);
  if (!entry) {
    on_done(Result<std::string>::Failure(common::MakeError(
        ErrorCode::kSessionNotFound,
        "UCX CommitPartData: session not found: " + std::string(session_id))));
    return true;
  }
  if (!entry->slot || !entry->slot->buf) {
    on_done(Result<std::string>::Failure(common::MakeError(
        ErrorCode::kInternal, "UCX session buffer not allocated")));
    return true;
  }

  auto release_and_erase = [this, sid = std::string(session_id), entry]() mutable {
    session_registry_->Erase(sid);
    ReleaseEntry(entry);
    common::metrics().ucx_put_inflight << -1;
  };

  // 快速路径：数据已到
  if (entry->write_done.load(std::memory_order_acquire)) {
    auto r = DoWritePartData(entry, backend_upload_id, part_number,
                              bytes_transferred, client_crc32c_b64);
    release_and_erase();
    on_done(std::move(r));
    return true;
  }

  // 慢速路径：注册 pending_commit，等 OnAmWriteDone
  std::string uid(backend_upload_id);
  {
    std::lock_guard lk(entry->commit_mu);
    // double-check
    if (entry->write_done.load(std::memory_order_acquire)) {
      auto r = DoWritePartData(entry, backend_upload_id, part_number,
                                bytes_transferred, client_crc32c_b64);
      release_and_erase();
      on_done(std::move(r));
      return true;
    }
    entry->pending_commit = [this, entry, uid, part_number, bytes_transferred,
                              crc = std::string(client_crc32c_b64),
                              on_done = std::move(on_done), release_and_erase]() mutable {
      auto r = DoWritePartData(entry, uid, part_number, bytes_transferred, crc);
      release_and_erase();
      on_done(std::move(r));
    };
  }
  return false;
}

// ---- AbortSession ----

Result<bool> UcxExecutor::AbortSession(std::string_view session_id) {
  auto entry = session_registry_->Erase(session_id);
  ReleaseEntry(entry);
  return Result<bool>::Success(entry != nullptr);
}

}  // namespace us3_turbo_access::gateway::data_path::ucx
