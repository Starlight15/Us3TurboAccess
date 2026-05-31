#include "client/src/core/rdma/rdma_transfer_path.h"

#include <chrono>
#include <future>
#include <span>
#include <utility>

#include "client/src/data/http_crc32c.h"  // Crc32c / Base64Crc32cBigEndian (三通路共享)

#include "client/src/core/common/errors.h"
#include "client/src/core/contracts/request_builder.h"
#include "client/src/core/routing/retry_policy.h"
#include "client/src/transports/rdma/host_memory_registry.h"
#include "client/src/transports/rdma/rdma_cm_connection.h"
#include "client/src/transports/rdma/rdma_completion_driver.h"
#include "client/src/transports/rdma/rdma_connection_pool.h"
#include "client/src/transports/rdma/rdma_transfer_engine.h"

namespace us3_turbo_access::client {

// driver 和 pool 的成员声明顺序决定析构次序：pool_ 先于 driver_ 析构。
// pool 析构会逐条 Detach driver；若顺序反了，pool 关闭时 driver 已死，Detach
// 永久挂在 epoch 等待上。析构函数显式 reset 也是同样意图，并使依赖明文化。
RdmaTransferPath::RdmaTransferPath(const ClientOptions& options,
                                    const MetadataClient& metadata_client,
                                    const RdmaDataPlaneClient& data_plane_client)
    : options_(options),
      metadata_client_(metadata_client),
      data_plane_client_(data_plane_client),
      driver_(std::make_unique<RdmaCompletionDriver>()),
      pool_(std::make_unique<RdmaConnectionPool>(options.rdma, *driver_)) {
  // 先无条件起 driver：SetAvailable(true) 之后第一笔 PUT 立刻能 RegisterInflight，
  // 不需要再启动线程；空转代价 ≈ 50us sleep/轮。
  driver_->Start();
}

RdmaTransferPath::~RdmaTransferPath() {
  // 必须先 pool→driver：见类构造头注释。
  pool_.reset();
  driver_.reset();
}

void RdmaTransferPath::SetAvailable(bool available) noexcept {
  available_.store(available, std::memory_order_release);
}

DataPath RdmaTransferPath::path() const { return DataPath::kNativeRdma; }

bool RdmaTransferPath::available() const {
  return available_.load(std::memory_order_acquire);
}

Result<TransferOutcome> RdmaTransferPath::GetObject(const RequestOptions&,
                                                    MutableBufferView) const {
  return Result<TransferOutcome>::Failure(MakeUnsupportedPath(
      DataPath::kNativeRdma, "RDMA GET not implemented yet"));
}

namespace {

/*
 * PUT / PUT-part 共享的"开 session + 写完一次 RDMA WRITE"流程产物。Commit
 * 留给调用方做，因为 CommitObject 与 CommitPart 是两个不同的 RPC。
 * handle 在 Commit 成功后随作用域归还连接池复用；Commit 失败时 caller 调
 * AbortSession + 让 handle 自然析构归池（连接本身仍可用）。
 */
struct RdmaWritePrepared {
  std::string                                       session_id;
  std::string                                       request_id;
  std::string                                       gateway_id;
  ConnectionHandle                                  handle;
};

// 写阶段失败的统一收尾：丢弃连接（不污染池子）+ 通知 server 立即释放 session 的
// buffer/MR。AbortSession 自身的错误不向上抛——已经在错误路径上，不能掩盖原因。
void DiscardAndAbort(ConnectionHandle& handle,
                       const RdmaDataPlaneClient& data_plane,
                       const std::string& session_id) {
  handle.Discard();
  if (!session_id.empty()) {
    (void)data_plane.AbortSession(session_id);
  }
}

/*
 * PUT 主链路。各步必须按顺序，前后依赖：
 *   1 OpenSession      → 拿 session_id（之后失败必须 AbortSession 通知 server）
 *   2 DiscoverEndpoint → 拿 host:port（server-side RDMA listener 地址）
 *   3 pool.Acquire     → 借/建一条 QP；池命中可省去 CM 三次握手
 *   4 BindSession      → 在该 QP 的 PD 上为本 session 分配 buffer + 注册 MR，
 *                        返回 raddr/rkey；本步必须在 5 之前，因为 MR 要用 conn.pd()
 *   5 reg MR           → 把客户端 buffer 注册到同一 PD 上，得到 lkey
 *   6 PostWrite+wait   → 提交 IBV_WR_RDMA_WRITE，等 driver 派发 WC
 *
 * Commit 不在这里做：CommitObject 与 CommitPart 是不同 RPC，留给上层。
 * 失败收尾：第 1 步后任何失败都需要 AbortSession；第 3 步后还要 handle.Discard
 * 把该 QP 丢出池子（远端状态可能已脏）。
 */
[[nodiscard]] Result<RdmaWritePrepared> PrepareAndWrite(
    const ClientOptions& options, const MetadataClient& metadata,
    const RdmaDataPlaneClient& data_plane, RdmaConnectionPool& pool,
    RdmaCompletionDriver& driver, const RequestOptions& request,
    ConstBufferView buffer, bool is_multipart_part) {
  // 端到端 deadline：在 OpenSession / Discover / Bind 之间检查；
  // CQ 等待（wait_for）也用 deadline 做超时返回。
  const auto deadline =
      std::chrono::steady_clock::now() + options.request_timeout;
  auto check_deadline = [&](const char* stage) -> Result<RdmaWritePrepared> {
    if (std::chrono::steady_clock::now() >= deadline) {
      return Result<RdmaWritePrepared>::Failure(MakeError(
          ErrorCode::kTimeout,
          std::string("RDMA request_timeout exceeded before ") + stage, true));
    }
    return Result<RdmaWritePrepared>::Success({});
  };

  if (auto d = check_deadline("OpenSession"); !d.success()) return d;
  // 1) OpenSession
  auto open_req = MakeSessionHandshake(options, SessionPlan{
      .operation = OperationType::kPut,
      .request = request,
      .buffer_type = buffer.type,
      .path = DataPath::kNativeRdma,
      .is_multipart_part = is_multipart_part,
  });
  auto open_resp = metadata.OpenTransferSession(open_req);
  if (!open_resp.success()) {
    // 这里 server 还没开 session，无需 AbortSession。
    return Result<RdmaWritePrepared>::Failure(open_resp.error());
  }
  const std::string session_id = open_resp.value().session_id();

  if (auto d = check_deadline("DiscoverEndpoint"); !d.success()) {
    (void)data_plane.AbortSession(session_id);
    return d;
  }
  // 2) DiscoverEndpoint：拿到地址即可发起 CM；max_msg_bytes 用来提前拒掉超大请求。
  auto disc = data_plane.DiscoverEndpoint(session_id);
  if (!disc.success()) {
    (void)data_plane.AbortSession(session_id);
    return Result<RdmaWritePrepared>::Failure(disc.error());
  }
  if (buffer.size > disc.value().max_msg_bytes) {
    (void)data_plane.AbortSession(session_id);
    return Result<RdmaWritePrepared>::Failure(MakeInvalidArgument(
        "buffer 超过 gateway max_msg_bytes"));
  }

  if (auto d = check_deadline("ConnectionPool.Acquire"); !d.success()) {
    (void)data_plane.AbortSession(session_id);
    return d;
  }
  // 3) 借 QP：池里有空闲直接拿；没有就 CM 三次握手现场建。
  auto handle = pool.Acquire(disc.value().host,
                              static_cast<std::uint16_t>(disc.value().port));
  if (!handle.success()) {
    (void)data_plane.AbortSession(session_id);
    return Result<RdmaWritePrepared>::Failure(handle.error());
  }
  ConnectionHandle conn_handle = std::move(handle.value());
  auto* conn = conn_handle.conn();

  if (auto d = check_deadline("BindSession"); !d.success()) {
    DiscardAndAbort(conn_handle, data_plane, session_id);
    return d;
  }
  // 4) Bind：server 收到 conn_token 后在该连接的 PD 上 alloc + reg_mr，
  //    把 (raddr, rkey) 回给我们当 RDMA WRITE 的目的地。
  auto bound = data_plane.BindSessionToConnection(session_id, conn->conn_token());
  if (!bound.success()) {
    DiscardAndAbort(conn_handle, data_plane, session_id);
    return Result<RdmaWritePrepared>::Failure(bound.error());
  }
  conn->set_remote_buffer(bound.value().raddr, bound.value().rkey);

  // 5) 在 conn 的 PD 上注册本地 buffer 拿 lkey；MR 在本函数返回前不能析构。
  HostMemoryRegistry mr_registry(conn->pd());
  auto mr = mr_registry.Register(buffer);
  if (!mr.success()) {
    DiscardAndAbort(conn_handle, data_plane, session_id);
    return Result<RdmaWritePrepared>::Failure(mr.error());
  }

  // 6) 注意顺序：必须先 RegisterInflight 抢到 promise，再 PostWrite；
  //    否则 driver 可能在我们 RegisterInflight 之前就 poll 到 WC，promise 缺失。
  const auto wr_id = conn_handle.next_wr_id();
  auto fut = driver.RegisterInflight(conn_handle.driver_conn_id(), wr_id);
  auto posted = RdmaTransferEngine::PostWrite(
      conn->qp(), mr.value(), static_cast<std::uint64_t>(buffer.size),
      bound.value().raddr, bound.value().rkey, wr_id);
  if (!posted.success()) {
    DiscardAndAbort(conn_handle, data_plane, session_id);
    return Result<RdmaWritePrepared>::Failure(posted.error());
  }
  // 阻塞等 WC：用 deadline 控制 CQ 等待时长；超时则丢弃 QP（远端可能仍在写）。
  RdmaCompletion comp{};
  if (fut.wait_until(deadline) != std::future_status::ready) {
    DiscardAndAbort(conn_handle, data_plane, session_id);
    return Result<RdmaWritePrepared>::Failure(MakeError(
        ErrorCode::kTimeout,
        "RDMA WRITE completion timeout (request_timeout exceeded)", true,
        std::string(ToString(DataPath::kNativeRdma))));
  }
  try {
    comp = fut.get();
  } catch (const std::exception& e) {
    // 通常意味着 driver Stop / 连接被 Detach；继续也无意义。
    DiscardAndAbort(conn_handle, data_plane, session_id);
    return Result<RdmaWritePrepared>::Failure(MakeTransportFailure(
        std::string("RDMA completion driver error: ") + e.what(),
        DataPath::kNativeRdma, "", false));
  }
  if (comp.status != 0) {
    // IBV_WC_SUCCESS==0；非 0 状态对 QP 来说基本是 fatal，连接不能再复用。
    DiscardAndAbort(conn_handle, data_plane, session_id);
    return Result<RdmaWritePrepared>::Failure(MakeTransportFailure(
        std::string("RDMA WRITE failed status=") + std::to_string(comp.status),
        DataPath::kNativeRdma, "", false));
  }

  RdmaWritePrepared out;
  out.session_id = session_id;
  out.request_id = open_resp.value().request_id();
  out.gateway_id = open_resp.value().gateway_id();
  // handle 沿着 out move 出去；只有 Commit 路径才决定它最终是归池还是丢弃。
  out.handle     = std::move(conn_handle);
  return Result<RdmaWritePrepared>::Success(std::move(out));
}

// RDMA 路径要求页锁定 host 内存：ibv_reg_mr 会失败在普通 malloc 的内存上
// （内核 pin 时拿不到稳定物理页）。这里前置拒绝，避免走到 5) reg_mr 才报错。
[[nodiscard]] Result<bool> PreflightRdma(bool available, BufferType buffer_type) {
  if (!available) {
    return Result<bool>::Failure(MakeUnsupportedPath(
        DataPath::kNativeRdma, "RDMA resources not initialized"));
  }
  if (buffer_type != BufferType::kHostPinned) {
    return Result<bool>::Failure(MakeUnsupportedPath(
        DataPath::kNativeRdma,
        "RDMA path requires kHostPinned buffer; "
        "use PinnedBuffer::Allocate or self-pinned mlock'd memory"));
  }
  return Result<bool>::Success(true);
}

}  // namespace

// 整对象 PUT：写完 = WRITE 完成 + CommitObject 通知 server 落盘。
// Commit 失败时 server 端 session 仍持有 buffer/MR，必须 AbortSession 释放。
// 函数返回后 prepared.handle 析构 → 连接归池复用（QP/PD/CQ 都保留）。
Result<TransferOutcome> RdmaTransferPath::PutObject(const RequestOptions& request,
                                                     ConstBufferView buffer) const {
  auto pre = PreflightRdma(available(), buffer.type);
  if (!pre.success()) return Result<TransferOutcome>::Failure(pre.error());

  // 入口拒：与 NIC max_msg 上限对齐（之前要等 DiscoverEndpoint 拿到 server
  // 端 max_msg_bytes 才校验，一来一回浪费一次 RPC）。HTTP put_single_max_bytes
  // 是同款思路。
  if (buffer.size > options_.rdma.max_msg_bytes) {
    return Result<TransferOutcome>::Failure(MakeError(
        ErrorCode::kPayloadTooLarge,
        "RDMA PUT body " + std::to_string(buffer.size) +
            " exceeds rdma.max_msg_bytes " +
            std::to_string(options_.rdma.max_msg_bytes) +
            "; use multipart upload",
        /*retryable=*/false));
  }

  // PUT 是幂等覆写（gateway 按 bucket/key 整体覆盖），retryable 错误自动重试。
  // 每次尝试都重做 PrepareAndWrite（OpenSession+WRITE）：session_id / MR 都是
  // 一次性的，重试必须重新走完整流程。
  return RetryIfRetryable(DefaultRetryPolicy(), [&]() -> Result<TransferOutcome> {
    auto prepared = PrepareAndWrite(options_, metadata_client_, data_plane_client_,
                                      *pool_, *driver_, request, buffer,
                                      /*is_multipart_part=*/false);
    if (!prepared.success()) {
      return Result<TransferOutcome>::Failure(prepared.error());
    }

    // 算 client CRC32C（host pinned buffer，与 server 端 RDMA WRITE 到的
    // pinned buffer 字节相同），CommitObject 时让 server 校验。
    std::string client_crc_b64;
    if (options_.rdma.send_crc32c && buffer.size > 0 && buffer.data != nullptr) {
      const auto crc = Crc32c(std::span<const std::byte>(
          static_cast<const std::byte*>(buffer.data), buffer.size));
      client_crc_b64 = Base64Crc32cBigEndian(crc);
    }

    auto commit = data_plane_client_.CommitObject(
        prepared.value().session_id,
        static_cast<std::uint64_t>(buffer.size),
        client_crc_b64);
    if (!commit.success()) {
      (void)data_plane_client_.AbortSession(prepared.value().session_id);
      return Result<TransferOutcome>::Failure(commit.error());
    }

    TransferOutcome outcome;
    outcome.selected_path     = DataPath::kNativeRdma;
    outcome.request_id        = std::move(prepared.value().request_id);
    outcome.session_id        = std::move(prepared.value().session_id);
    outcome.gateway_id        = std::move(prepared.value().gateway_id);
    outcome.transfer_status   = "completed";
    outcome.etag              = commit.value().etag;
    outcome.version           = commit.value().version;
    outcome.bytes_transferred = buffer.size;
    // RDMA WRITE 完成 + Commit 通过后回调一次（中间不可中断，无逐步进度）。
    if (request.progress_callback) {
      request.progress_callback({.bytes_completed = buffer.size,
                                  .bytes_total = buffer.size,
                                  .data_path = DataPath::kNativeRdma});
    }
    return Result<TransferOutcome>::Success(std::move(outcome));
  });
}

// Multipart 单 part 上传：流程完全等同 PutObject，区别只在 Commit RPC：
//   - CommitPart 把字节按 (upload_id, part_number) 写入临时分片，不立即拼对象
//   - upload_id / part_number 由调用方传入（part_number>=1）
//   - is_multipart_part=true 让 server 在 OpenSession 阶段跳过整对象 Reserve
Result<TransferOutcome> RdmaTransferPath::PutObjectPart(
    const RequestOptions& request, ConstBufferView buffer,
    const std::string& upload_id, std::uint32_t part_number) const {
  auto pre = PreflightRdma(available(), buffer.type);
  if (!pre.success()) return Result<TransferOutcome>::Failure(pre.error());
  if (upload_id.empty() || part_number == 0) {
    return Result<TransferOutcome>::Failure(MakeInvalidArgument(
        "PutObjectPart 需要非空 upload_id 与 part_number>=1"));
  }

  // 入口拒：part size 同样受 max_msg_bytes 约束。
  if (buffer.size > options_.rdma.max_msg_bytes) {
    return Result<TransferOutcome>::Failure(MakeError(
        ErrorCode::kPayloadTooLarge,
        "RDMA UploadPart body " + std::to_string(buffer.size) +
            " exceeds rdma.max_msg_bytes " +
            std::to_string(options_.rdma.max_msg_bytes),
        /*retryable=*/false));
  }

  // 单 part 上传幂等（同 part_number 覆盖），可重试。
  return RetryIfRetryable(DefaultRetryPolicy(), [&]() -> Result<TransferOutcome> {
    auto prepared = PrepareAndWrite(options_, metadata_client_, data_plane_client_,
                                      *pool_, *driver_, request, buffer,
                                      /*is_multipart_part=*/true);
    if (!prepared.success()) {
      return Result<TransferOutcome>::Failure(prepared.error());
    }

    // 算 client CRC32C（与 PutObject 路径同构）让 server 端 CommitPart 校验。
    std::string client_crc_b64;
    if (options_.rdma.send_crc32c && buffer.size > 0 && buffer.data != nullptr) {
      const auto crc = Crc32c(std::span<const std::byte>(
          static_cast<const std::byte*>(buffer.data), buffer.size));
      client_crc_b64 = Base64Crc32cBigEndian(crc);
    }

    auto commit = data_plane_client_.CommitPart(
        prepared.value().session_id, upload_id, part_number,
        static_cast<std::uint64_t>(buffer.size),
        client_crc_b64);
    if (!commit.success()) {
      (void)data_plane_client_.AbortSession(prepared.value().session_id);
      return Result<TransferOutcome>::Failure(commit.error());
    }

    TransferOutcome outcome;
    outcome.selected_path     = DataPath::kNativeRdma;
    outcome.request_id        = std::move(prepared.value().request_id);
    outcome.session_id        = std::move(prepared.value().session_id);
    outcome.gateway_id        = std::move(prepared.value().gateway_id);
    outcome.transfer_status   = "completed";
    outcome.etag              = commit.value().part_etag;
    outcome.bytes_transferred = buffer.size;
    // Multipart 单 part 完成回调（上层 UploadParts 串起多 part 进度）。
    if (request.progress_callback) {
      request.progress_callback({.bytes_completed = buffer.size,
                                  .bytes_total = buffer.size,
                                  .data_path = DataPath::kNativeRdma});
    }
    return Result<TransferOutcome>::Success(std::move(outcome));
  });
}

}  // namespace us3_turbo_access::client
