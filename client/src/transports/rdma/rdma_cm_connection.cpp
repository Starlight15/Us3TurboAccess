#include "client/src/transports/rdma/rdma_cm_connection.h"

#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <rdma/rdma_cma.h>

#include <chrono>
#include <cstring>
#include <utility>

#include "client/src/core/common/errors.h"
#include "us3_turbo_access/common/rdma_wire.h"

namespace us3_turbo_access::client {

namespace {

[[nodiscard]] Result<bool> WaitForEvent(rdma_event_channel* ec,
                                         rdma_cm_event_type expected,
                                         std::chrono::milliseconds timeout) {
  // 简化：阻塞 rdma_get_cm_event；timeout 暂未严格 enforce。
  (void)timeout;
  rdma_cm_event* ev = nullptr;
  if (rdma_get_cm_event(ec, &ev) != 0 || ev == nullptr) {
    return Result<bool>::Failure(MakeTransportFailure(
        "rdma_get_cm_event failed", DataPath::kNativeRdma, "", false));
  }
  const auto type = ev->event;
  (void)rdma_ack_cm_event(ev);
  if (type != expected) {
    return Result<bool>::Failure(MakeTransportFailure(
        std::string("unexpected CM event ") + std::to_string(static_cast<int>(type)),
        DataPath::kNativeRdma, "", false));
  }
  return Result<bool>::Success(true);
}

}  // namespace

RdmaCmConnection::RdmaCmConnection(const RdmaClientOptions& opts)
    : opts_(opts) {}

RdmaCmConnection::~RdmaCmConnection() { Disconnect(); }

ibv_qp* RdmaCmConnection::qp() const noexcept {
  return cm_id_ != nullptr ? cm_id_->qp : nullptr;
}

/*
 * 客户端发起一次 CM 三次握手并建出可用的 QP。完整步骤：
 *   1 create_event_channel + create_id：rdma_cm 异步事件通道。
 *   2 resolve_addr / resolve_route：解析远端 + 路由，到 ROUTE_RESOLVED 后
 *     cm_id->verbs 才能确定（rdma_cm 内部按路由选 RDMA 设备）。
 *   3 在 cm_id->verbs 上 alloc PD / create CQ：PD 的 context 必须跟 cm_id
 *     的 verbs 完全一致，否则 rdma_create_qp 会失败。
 *   4 create_qp + connect。
 *   5 等 ESTABLISHED：private_data 由 server rdma_accept 时写入，
 *     里面带 conn_token，本端拿到后才能调 BindSessionToConnection RPC。
 *
 * 失败路径不在这里收尾资源；Disconnect 析构时会清理半成品。
 */
Result<bool> RdmaCmConnection::Connect(const std::string& host,
                                        std::uint16_t port) {
  if (connected_) return Result<bool>::Success(true);

  event_channel_ = rdma_create_event_channel();
  if (event_channel_ == nullptr) {
    return Result<bool>::Failure(MakeTransportFailure(
        "rdma_create_event_channel failed", DataPath::kNativeRdma, "", false));
  }
  if (rdma_create_id(event_channel_, &cm_id_, nullptr, RDMA_PS_TCP) != 0) {
    return Result<bool>::Failure(MakeTransportFailure(
        "rdma_create_id failed", DataPath::kNativeRdma, "", false));
  }

  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port   = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &dst.sin_addr) != 1) {
    return Result<bool>::Failure(MakeTransportFailure(
        "host parse failed: " + host, DataPath::kNativeRdma, "", false));
  }

  if (rdma_resolve_addr(cm_id_, nullptr, reinterpret_cast<sockaddr*>(&dst),
                         /*timeout_ms=*/2000) != 0) {
    return Result<bool>::Failure(MakeTransportFailure(
        "rdma_resolve_addr failed", DataPath::kNativeRdma, "", false));
  }
  auto r1 = WaitForEvent(event_channel_, RDMA_CM_EVENT_ADDR_RESOLVED,
                          opts_.connect_timeout);
  if (!r1.success()) return r1;

  if (rdma_resolve_route(cm_id_, /*timeout_ms=*/2000) != 0) {
    return Result<bool>::Failure(MakeTransportFailure(
        "rdma_resolve_route failed", DataPath::kNativeRdma, "", false));
  }
  auto r2 = WaitForEvent(event_channel_, RDMA_CM_EVENT_ROUTE_RESOLVED,
                          opts_.connect_timeout);
  if (!r2.success()) return r2;

  // 用 cm_id 路由选中的 verbs context 建 PD/CQ。
  // 这是关键：rdma_create_qp 要求 PD 的 context 与 cm_id->verbs 完全一致，
  // 否则触发 "rdma_create_qp failed"。
  pd_ = ibv_alloc_pd(cm_id_->verbs);
  if (pd_ == nullptr) {
    return Result<bool>::Failure(MakeTransportFailure(
        "ibv_alloc_pd failed", DataPath::kNativeRdma, "", false));
  }
  cq_ = ibv_create_cq(cm_id_->verbs, /*cqe=*/opts_.cq_size, nullptr, nullptr, 0);
  if (cq_ == nullptr) {
    return Result<bool>::Failure(MakeTransportFailure(
        "ibv_create_cq failed", DataPath::kNativeRdma, "", false));
  }

  ibv_qp_init_attr qp_attr{};
  qp_attr.send_cq          = cq_;
  qp_attr.recv_cq          = cq_;
  qp_attr.qp_type          = IBV_QPT_RC;
  qp_attr.sq_sig_all       = 0;
  qp_attr.cap.max_send_wr  = 16;
  qp_attr.cap.max_recv_wr  = 16;
  qp_attr.cap.max_send_sge = 4;
  qp_attr.cap.max_recv_sge = 4;
  if (rdma_create_qp(cm_id_, pd_, &qp_attr) != 0) {
    return Result<bool>::Failure(MakeTransportFailure(
        "rdma_create_qp failed", DataPath::kNativeRdma, "", false));
  }

  rdma_conn_param cp{};
  cp.responder_resources = 1;
  cp.initiator_depth     = 1;
  cp.rnr_retry_count     = 7;
  cp.retry_count         = 7;
  if (rdma_connect(cm_id_, &cp) != 0) {
    return Result<bool>::Failure(MakeTransportFailure(
        "rdma_connect failed", DataPath::kNativeRdma, "", false));
  }

  // ESTABLISHED 事件里取 conn_token；rdma_ack_cm_event 之后 private_data
  // 指针会被释放，必须先 memcpy 出来再 ack。这一段没用 WaitForEvent helper
  // 是因为要在 ack 前读 ev->param.conn.private_data。
  rdma_cm_event* ev = nullptr;
  if (rdma_get_cm_event(event_channel_, &ev) != 0 || ev == nullptr) {
    return Result<bool>::Failure(MakeTransportFailure(
        "rdma_get_cm_event failed (waiting ESTABLISHED)",
        DataPath::kNativeRdma, "", false));
  }
  const auto ev_type = ev->event;
  if (ev_type != RDMA_CM_EVENT_ESTABLISHED) {
    (void)rdma_ack_cm_event(ev);
    return Result<bool>::Failure(MakeTransportFailure(
        std::string("unexpected CM event ") + std::to_string(static_cast<int>(ev_type)),
        DataPath::kNativeRdma, "", false));
  }
  // magic + version 双重校验：避免对接到非本服务的 RDMA 端导致误读。
  const auto* priv = static_cast<const std::uint8_t*>(ev->param.conn.private_data);
  const auto  priv_len = ev->param.conn.private_data_len;
  bool got_token = false;
  if (priv != nullptr && priv_len >= sizeof(::us3_turbo_access::common::RdmaConnectCredentials)) {
    ::us3_turbo_access::common::RdmaConnectCredentials creds{};
    std::memcpy(&creds, priv, sizeof(creds));
    if (creds.magic == ::us3_turbo_access::common::kRdmaCredentialsMagic &&
        creds.version == ::us3_turbo_access::common::kRdmaCredentialsVersion) {
      conn_token_ = creds.conn_token;
      got_token = true;
    }
  }
  (void)rdma_ack_cm_event(ev);
  if (!got_token) {
    return Result<bool>::Failure(MakeTransportFailure(
        "ESTABLISHED missing/invalid private_data conn_token",
        DataPath::kNativeRdma, "", false));
  }

  connected_ = true;
  return Result<bool>::Success(true);
}

// 资源销毁严格按依赖反序：QP 引用 PD/CQ，cm_id 引用 QP。
// 顺序：disconnect → destroy_qp → destroy_id → destroy_cq → dealloc_pd → channel。
// 调用方（pool / handle）必须先 driver.Detach 确认 driver 不再 poll 该 CQ
// 再调本函数，否则 ibv_destroy_cq 与 driver poll race 是 UB。
void RdmaCmConnection::Disconnect() {
  if (cm_id_ != nullptr) {
    if (connected_) {
      rdma_disconnect(cm_id_);
      connected_ = false;
    }
    if (cm_id_->qp != nullptr) {
      rdma_destroy_qp(cm_id_);
    }
    rdma_destroy_id(cm_id_);
    cm_id_ = nullptr;
  }
  if (cq_ != nullptr) { (void)ibv_destroy_cq(cq_); cq_ = nullptr; }
  if (pd_ != nullptr) { (void)ibv_dealloc_pd(pd_); pd_ = nullptr; }
  if (event_channel_ != nullptr) {
    rdma_destroy_event_channel(event_channel_);
    event_channel_ = nullptr;
  }
}

}  // namespace us3_turbo_access::client
