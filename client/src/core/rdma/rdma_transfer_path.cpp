#include "client/src/core/rdma/rdma_transfer_path.h"

#include <chrono>
#include <span>

#include "client/src/core/common/crc32c_helper.h"
#include "client/src/core/common/errors.h"
#include "client/src/core/contracts/request_builder.h"
#include "client/src/core/routing/retry_policy.h"
#include "client/src/data/http_crc32c.h"
#include "client/src/transports/ucx/ucx_endpoint.h"

namespace us3_turbo_access::client {

RdmaTransferPath::RdmaTransferPath(const ClientOptions& options,
                                    const MetadataClient& metadata_client,
                                    const RdmaDataPlaneClient& data_plane_client)
    : options_(options),
      metadata_client_(metadata_client),
      data_plane_client_(data_plane_client) {
  auto ctx = UcxContext::Create();
  if (!ctx.success()) return;
  auto wkr = UcxWorker::Create(*ctx.value());
  if (!wkr.success()) return;
  ucx_ctx_    = std::move(ctx.value());
  ucx_worker_ = std::move(wkr.value());
  ucx_pool_   = std::make_unique<UcxEndpointPool>(
      *ucx_worker_, options_.rdma.pool_max_idle_per_endpoint);
}

RdmaTransferPath::~RdmaTransferPath() {
  ucx_pool_.reset();   // 先关闭所有 idle ep
  ucx_worker_.reset();
  ucx_ctx_.reset();
}

void RdmaTransferPath::SetAvailable(bool available) noexcept {
  available_.store(available, std::memory_order_release);
}

DataPath RdmaTransferPath::path() const { return DataPath::kNativeRdma; }

bool RdmaTransferPath::available() const {
  return available_.load(std::memory_order_acquire) &&
         ucx_ctx_ != nullptr && ucx_worker_ != nullptr && ucx_pool_ != nullptr;
}

Result<TransferOutcome> RdmaTransferPath::GetObject(const RequestOptions&,
                                                     MutableBufferView) const {
  return Result<TransferOutcome>::Failure(
      MakeUnsupportedPath(DataPath::kNativeRdma, "RDMA GET not implemented yet"));
}

void RdmaTransferPath::ReturnEp(WritePrepared& prepared, bool success) const noexcept {
  if (!prepared.slot.ep) return;
  if (success) {
    ucx_pool_->Return(prepared.ep_host, prepared.ep_port, prepared.slot);
  } else {
    ucx_pool_->Discard(prepared.slot);
  }
  prepared.slot = {};
}
// ---- PrepareAndWrite（UCX RMA WRITE 路径）----
//
// 1. OpenSession RPC
// 2. PrepareTransfer RPC（只传 transfer_bytes）→ gateway 分配 gw_buf+注册 MR
//    → 返回 ucx_port / gw_raddr / gw_packed_rkey
// 3. ucp_ep_create(ucx_port) → ep
// 4. ucp_ep_rkey_unpack(gw_packed_rkey) → gw_rkey
// 5. PutAndNotify: ucp_put_nbx(gw_raddr) + ep_flush + AM_WRITE_DONE
//    → gateway write_done 信号置位

Result<RdmaTransferPath::WritePrepared>
RdmaTransferPath::PrepareAndWrite(const RequestOptions& request,
                                   ConstBufferView buffer,
                                   bool is_multipart_part,
                                   std::chrono::steady_clock::time_point deadline) const {
  auto check_deadline = [&]() -> bool {
    return std::chrono::steady_clock::now() < deadline;
  };
  auto timeout_err = [](const char* stage) -> Result<WritePrepared> {
    return Result<WritePrepared>::Failure(MakeError(
        ErrorCode::kTimeout,
        std::string("UCX RMA timeout before ") + stage, true));
  };

  // 1) OpenSession
  if (!check_deadline()) return timeout_err("OpenSession");
  auto open_resp = metadata_client_.RpcOpenTransferSession(
      MakeSessionHandshake(options_, SessionPlan{
          .operation         = OperationType::kPut,
          .request           = request,
          .buffer_type       = buffer.type,
          .path              = DataPath::kNativeRdma,
          .is_multipart_part = is_multipart_part,
      }));
  if (!open_resp.success())
    return Result<WritePrepared>::Failure(open_resp.error());

  const std::string session_id = open_resp.value().session_id();
  const std::string request_id = open_resp.value().request_id();
  const std::string gateway_id = open_resp.value().gateway_id();

  // 2) PrepareTransfer：gateway 分配+注册 gw_buf，下发 gw_raddr/gw_packed_rkey
  if (!check_deadline()) {
    (void)data_plane_client_.AbortSession(session_id);
    return timeout_err("PrepareTransfer");
  }
  auto disc = data_plane_client_.PrepareTransfer(
      session_id, static_cast<std::uint64_t>(buffer.size));
  if (!disc.success()) {
    (void)data_plane_client_.AbortSession(session_id);
    return Result<WritePrepared>::Failure(disc.error());
  }
  if (disc.value().ucx_port == 0 || disc.value().gw_raddr == 0) {
    (void)data_plane_client_.AbortSession(session_id);
    return Result<WritePrepared>::Failure(MakeTransportFailure(
        "gateway UCX not enabled or gw_raddr is zero",
        DataPath::kNativeRdma, session_id, false));
  }

  // 3) 从 pool 获取 ep（命中则复用，miss 则新建握手）
  if (!check_deadline()) {
    (void)data_plane_client_.AbortSession(session_id);
    return timeout_err("pool.Acquire");
  }

  const std::string& gw_host = disc.value().host;
  const uint16_t     gw_port = static_cast<uint16_t>(disc.value().ucx_port);

  // 3) 从 pool 获取 slot（ep+rkey）；pool miss 时新建 ep 并 unpack rkey
  auto slot_res = ucx_pool_->Acquire(gw_host, gw_port, disc.value().gw_packed_rkey);
  if (!slot_res.success()) {
    (void)data_plane_client_.AbortSession(session_id);
    return Result<WritePrepared>::Failure(slot_res.error());
  }
  EpSlot slot = slot_res.value();

  // pool 命中时 rkey 已清空（防止跨 gateway 重启失效），需对本次 packed_rkey 重新 unpack
  if (!slot.rkey && !disc.value().gw_packed_rkey.empty()) {
    auto ust = ucp_ep_rkey_unpack(slot.ep, disc.value().gw_packed_rkey.data(), &slot.rkey);
    if (ust != UCS_OK) {
      ucx_pool_->Discard(slot);
      (void)data_plane_client_.AbortSession(session_id);
      return Result<WritePrepared>::Failure(MakeTransportFailure(
          std::string("ucp_ep_rkey_unpack: ") + ucs_status_string(ust),
          DataPath::kNativeRdma, session_id, false));
    }
  }
  if (!slot.rkey) {
    ucx_pool_->Discard(slot);
    (void)data_plane_client_.AbortSession(session_id);
    return Result<WritePrepared>::Failure(MakeTransportFailure(
        "gw_packed_rkey is empty", DataPath::kNativeRdma, session_id, false));
  }

  // 4) ucp_put_nbx + AM_WRITE_DONE（无 flush，progress loop 驱动）
  if (!check_deadline()) {
    ucx_pool_->Discard(slot);
    (void)data_plane_client_.AbortSession(session_id);
    return timeout_err("PutAndNotify");
  }

  auto put_fut = UcxEndpointOps::PutAndNotify(
      ucx_worker_->handle(), slot.ep,
      buffer.data, buffer.size,
      disc.value().gw_raddr, slot.rkey,
      session_id);

  // UcxWorker::ProgressLoop 后台线程已持续驱动 worker，无需调用方轮询
  if (put_fut.wait_until(deadline) != std::future_status::ready) {
    ucx_pool_->Discard(slot);
    (void)data_plane_client_.AbortSession(session_id);
    return Result<WritePrepared>::Failure(MakeError(
        ErrorCode::kTimeout, "PutAndNotify timeout", true,
        std::string(ToString(DataPath::kNativeRdma))));
  }

  auto put_status = put_fut.get();

  if (put_status != UCS_OK) {
    ucx_pool_->Discard(slot);
    (void)data_plane_client_.AbortSession(session_id);
    return Result<WritePrepared>::Failure(MakeTransportFailure(
        std::string("PutAndNotify failed: ") + ucs_status_string(put_status),
        DataPath::kNativeRdma, session_id, true));
  }

  // slot（ep+rkey）留给 ReturnEp 归还 pool
  return Result<WritePrepared>::Success(
      WritePrepared{session_id, request_id, gateway_id, gw_host, gw_port, slot});
}

// ---- PutObject ----

Result<TransferOutcome> RdmaTransferPath::PutObject(
    const RequestOptions& request, ConstBufferView buffer) const {
  if (!available())
    return Result<TransferOutcome>::Failure(
        MakeUnsupportedPath(DataPath::kNativeRdma, "UCX not initialized"));

  if (buffer.size > options_.rdma.max_msg_bytes)
    return Result<TransferOutcome>::Failure(MakeError(
        ErrorCode::kPayloadTooLarge,
        "PUT body exceeds rdma.max_msg_bytes; use multipart upload", false));

  const auto deadline = std::chrono::steady_clock::now() + options_.request_timeout;
  return RetryIfRetryable(DefaultRetryPolicy(), [&]() -> Result<TransferOutcome> {
    if (std::chrono::steady_clock::now() >= deadline)
      return Result<TransferOutcome>::Failure(MakeError(
          ErrorCode::kTimeout, "PutObject retry deadline exceeded", true));

    auto prepared = PrepareAndWrite(request, buffer, false, deadline);
    if (!prepared.success())
      return Result<TransferOutcome>::Failure(prepared.error());

    std::string client_crc_b64;
    if (auto crc = ComputeClientCrc32c(buffer, options_.rdma.send_crc32c); crc.has_value()) {
      client_crc_b64 = Base64Crc32cBigEndian(crc.value());
    }

    auto commit = data_plane_client_.CommitObject(
        prepared.value().session_id,
        static_cast<std::uint64_t>(buffer.size), client_crc_b64);

    ReturnEp(prepared.value(), commit.success());

    if (!commit.success()) {
      (void)data_plane_client_.AbortSession(prepared.value().session_id);
      return Result<TransferOutcome>::Failure(commit.error());
    }

    TransferOutcome out;
    out.selected_path     = DataPath::kNativeRdma;
    out.request_id        = std::move(prepared.value().request_id);
    out.session_id        = std::move(prepared.value().session_id);
    out.gateway_id        = std::move(prepared.value().gateway_id);
    out.transfer_status   = "completed";
    out.etag              = commit.value().etag;
    out.version           = commit.value().version;
    out.bytes_transferred = buffer.size;
    if (request.progress_callback)
      request.progress_callback({buffer.size, buffer.size, DataPath::kNativeRdma});
    return Result<TransferOutcome>::Success(std::move(out));
  });
}

// ---- PutObjectPart ----

Result<TransferOutcome> RdmaTransferPath::PutObjectPart(
    const RequestOptions& request, ConstBufferView buffer,
    const std::string& upload_id, std::uint32_t part_number) const {
  if (!available())
    return Result<TransferOutcome>::Failure(
        MakeUnsupportedPath(DataPath::kNativeRdma, "UCX not initialized"));

  if (upload_id.empty() || part_number == 0)
    return Result<TransferOutcome>::Failure(
        MakeInvalidArgument("PutObjectPart 需要非空 upload_id 与 part_number>=1"));

  if (buffer.size > options_.rdma.max_msg_bytes)
    return Result<TransferOutcome>::Failure(MakeError(
        ErrorCode::kPayloadTooLarge,
        "UploadPart body exceeds rdma.max_msg_bytes", false));

  const auto deadline = std::chrono::steady_clock::now() + options_.request_timeout;
  return RetryIfRetryable(DefaultRetryPolicy(), [&]() -> Result<TransferOutcome> {
    if (std::chrono::steady_clock::now() >= deadline)
      return Result<TransferOutcome>::Failure(MakeError(
          ErrorCode::kTimeout, "PutObjectPart retry deadline exceeded", true));

    auto prepared = PrepareAndWrite(request, buffer, true, deadline);
    if (!prepared.success())
      return Result<TransferOutcome>::Failure(prepared.error());

    std::string client_crc_b64;
    if (auto crc = ComputeClientCrc32c(buffer, options_.rdma.send_crc32c); crc.has_value()) {
      client_crc_b64 = Base64Crc32cBigEndian(crc.value());
    }

    auto commit = data_plane_client_.CommitPart(
        prepared.value().session_id, upload_id, part_number,
        static_cast<std::uint64_t>(buffer.size), client_crc_b64);

    ReturnEp(prepared.value(), commit.success());

    if (!commit.success()) {
      (void)data_plane_client_.AbortSession(prepared.value().session_id);
      return Result<TransferOutcome>::Failure(commit.error());
    }

    TransferOutcome out;
    out.selected_path     = DataPath::kNativeRdma;
    out.request_id        = std::move(prepared.value().request_id);
    out.session_id        = std::move(prepared.value().session_id);
    out.gateway_id        = std::move(prepared.value().gateway_id);
    out.transfer_status   = "completed";
    out.etag              = commit.value().part_etag;
    out.version           = "";
    out.bytes_transferred = buffer.size;
    if (request.progress_callback)
      request.progress_callback({buffer.size, buffer.size, DataPath::kNativeRdma});
    return Result<TransferOutcome>::Success(std::move(out));
  });
}

}  // namespace us3_turbo_access::client
