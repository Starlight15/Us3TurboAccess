#include "data_path/rdma/rdma_listener.h"

#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <rdma/rdma_cma.h>

#include <cstdint>
#include <string>
#include <utility>

#include "common/error.h"
#include "data_path/rdma/rdma_connection_registry.h"
#include "data_path/rdma/rdma_resources.h"
#include "runtime/io_worker_pool.h"
#include "us3_turbo_access/common/rdma_wire.h"

namespace us3_turbo_access::gateway::data_path::rdma {

namespace {

[[nodiscard]] Error MakeRdmaError(std::string message) {
  Error err;
  err.code = ErrorCode::kRdmaUnavailable;
  err.message = std::move(message);
  err.retryable = false;
  return err;
}

}  // namespace

RdmaListener::RdmaListener(std::shared_ptr<RdmaResources> resources,
                            RdmaConnectionRegistry* connection_registry,
                            const RdmaOptions& opts,
                            runtime::IoWorkerPool* io_pool,
                            std::shared_ptr<spdlog::logger> logger)
    : resources_(std::move(resources)),
      connection_registry_(connection_registry),
      opts_(opts),
      io_pool_(io_pool),
      logger_(std::move(logger)) {}

RdmaListener::~RdmaListener() { Stop(); }

Result<bool> RdmaListener::Start(const std::string& bind_host) {
  if (running_.load(std::memory_order_acquire)) {
    return Result<bool>::Success(true);
  }

  event_channel_ = rdma_create_event_channel();
  if (event_channel_ == nullptr) {
    return Result<bool>::Failure(
        MakeRdmaError("rdma_create_event_channel failed"));
  }

  if (rdma_create_id(event_channel_, &listen_id_, /*context=*/nullptr,
                      RDMA_PS_TCP) != 0) {
    rdma_destroy_event_channel(event_channel_);
    event_channel_ = nullptr;
    return Result<bool>::Failure(MakeRdmaError("rdma_create_id failed"));
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(static_cast<std::uint16_t>(opts_.listen_port));
  if (bind_host.empty() || bind_host == "0.0.0.0") {
    addr.sin_addr.s_addr = INADDR_ANY;
  } else {
    if (inet_pton(AF_INET, bind_host.c_str(), &addr.sin_addr) != 1) {
      return Result<bool>::Failure(
          MakeRdmaError("rdma bind_host parse failed: " + bind_host));
    }
  }

  if (rdma_bind_addr(listen_id_, reinterpret_cast<sockaddr*>(&addr)) != 0) {
    return Result<bool>::Failure(
        MakeRdmaError("rdma_bind_addr failed (port in use? wrong host?)"));
  }
  if (rdma_listen(listen_id_, /*backlog=*/16) != 0) {
    return Result<bool>::Failure(MakeRdmaError("rdma_listen failed"));
  }

  running_.store(true, std::memory_order_release);
  event_thread_ = std::thread(&RdmaListener::EventLoop, this);
  if (logger_ != nullptr) {
    logger_->info("rdma: listening on {}:{}", bind_host, opts_.listen_port);
  }
  return Result<bool>::Success(true);
}

void RdmaListener::Stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false,
                                          std::memory_order_acq_rel)) {
    return;
  }
  if (listen_id_ != nullptr) {
    rdma_destroy_id(listen_id_);
    listen_id_ = nullptr;
  }
  if (event_thread_.joinable()) {
    event_thread_.join();
  }
  if (event_channel_ != nullptr) {
    rdma_destroy_event_channel(event_channel_);
    event_channel_ = nullptr;
  }
  if (logger_ != nullptr) {
    logger_->info("rdma: listener stopped");
  }
}

void RdmaListener::EventLoop() {
  while (running_.load(std::memory_order_acquire)) {
    rdma_cm_event* ev = nullptr;
    const int rc = rdma_get_cm_event(event_channel_, &ev);
    if (rc != 0 || ev == nullptr) {
      break;
    }
    const auto type = ev->event;
    rdma_cm_id* id  = ev->id;
    (void)rdma_ack_cm_event(ev);

    switch (type) {
      case RDMA_CM_EVENT_CONNECT_REQUEST:
        // 派发到 io_pool：HandleConnectRequest 内部要 alloc PD/CQ/QP + accept，
        // 都是 kernel call，留在 event loop 会让 accept 串行。无 pool 时退化为
        // 同步执行（兜底）。
        if (io_pool_ != nullptr) {
          io_pool_->Submit([this, id]() { HandleConnectRequest(id); });
        } else {
          HandleConnectRequest(id);
        }
        break;
      case RDMA_CM_EVENT_ESTABLISHED:
        if (logger_ != nullptr) {
          logger_->debug("rdma: connection established id={}",
                         static_cast<const void*>(id));
        }
        break;
      case RDMA_CM_EVENT_DISCONNECTED:
        // 先 rdma_disconnect 完成对端 ack 握手，再回收本地资源。否则 RC QP
        // 半关，对端可能等到超时才感知。返回值忽略：已经在拆链路上。
        if (logger_ != nullptr) {
          logger_->debug("rdma: connection disconnected id={}",
                         static_cast<const void*>(id));
        }
        (void)rdma_disconnect(id);
        if (connection_registry_ != nullptr) {
          connection_registry_->EraseByCmId(id);
        }
        break;
      default:
        if (logger_ != nullptr) {
          logger_->warn("rdma: unhandled cm event type={}",
                        static_cast<int>(type));
        }
        break;
    }
  }
}

namespace {

// CONNECT_REQUEST 任一步失败的统一回收路径：记 error log → reject → 销毁 cm_id。
// 调用方负责按逆序回收已经申请的 pd / cq / qp（在 RejectAndDestroy 之前）。
void RejectAndDestroy(rdma_cm_id* id, spdlog::logger* logger, const char* what) {
  if (logger != nullptr) {
    logger->error("rdma: {}", what);
  }
  rdma_reject(id, nullptr, 0);
  rdma_destroy_id(id);
}

}  // namespace

/*
 * CONNECT_REQUEST 处理：只建连接级资源，不分配 session 级 buffer/MR。
 *   1. 在 cm_id->verbs 上 alloc PD / create CQ（必须同 context）
 *   2. rdma_create_qp（RC）
 *   3. 注册到 ConnectionRegistry 拿 conn_token
 *   4. ACCEPT 时把 conn_token 通过 private_data 回传给客户端
 *
 * 任一步失败 → 按逆序回收已申请的资源 + reject。
 */
void RdmaListener::HandleConnectRequest(rdma_cm_id* id) {
  if (connection_registry_ == nullptr) {
    RejectAndDestroy(id, logger_.get(), "connection registry is null");
    return;
  }

  ibv_pd* pd = ibv_alloc_pd(id->verbs);
  if (pd == nullptr) {
    RejectAndDestroy(id, logger_.get(), "ibv_alloc_pd failed");
    return;
  }
  // CQ 容量按 max_send_wr+max_recv_wr 给个宽松上限；实际服务端不主动发 WR，
  // 只接 client RDMA WRITE（不会进 CQ），CQ 主要是 QP 创建的形式要求。
  ibv_cq* cq = ibv_create_cq(id->verbs, /*cqe=*/64, /*ctx=*/nullptr,
                              /*channel=*/nullptr, /*comp_vector=*/0);
  if (cq == nullptr) {
    (void)ibv_dealloc_pd(pd);
    RejectAndDestroy(id, logger_.get(), "ibv_create_cq failed");
    return;
  }

  ibv_qp_init_attr qp_attr{};
  qp_attr.send_cq          = cq;
  qp_attr.recv_cq          = cq;
  qp_attr.qp_type          = IBV_QPT_RC;
  qp_attr.sq_sig_all       = 0;
  qp_attr.cap.max_send_wr  = 16;
  qp_attr.cap.max_recv_wr  = 16;
  qp_attr.cap.max_send_sge = 4;
  qp_attr.cap.max_recv_sge = 4;
  if (rdma_create_qp(id, pd, &qp_attr) != 0) {
    (void)ibv_destroy_cq(cq);
    (void)ibv_dealloc_pd(pd);
    RejectAndDestroy(id, logger_.get(), "rdma_create_qp failed");
    return;
  }

  const auto conn_token = connection_registry_->Register(id, pd, cq);

  ::us3_turbo_access::common::RdmaConnectCredentials creds{};
  creds.magic      = ::us3_turbo_access::common::kRdmaCredentialsMagic;
  creds.version    = ::us3_turbo_access::common::kRdmaCredentialsVersion;
  creds.conn_token = conn_token;

  rdma_conn_param params{};
  params.responder_resources = 1;
  params.initiator_depth     = 1;
  params.rnr_retry_count     = 7;
  params.private_data        = &creds;
  params.private_data_len    = sizeof(creds);
  if (rdma_accept(id, &params) != 0) {
    connection_registry_->EraseByCmId(id);  // 反向回收 + destroy cm_id
    if (logger_ != nullptr) {
      logger_->error("rdma: rdma_accept failed (conn_token={})", conn_token);
    }
    return;
  }

  if (logger_ != nullptr) {
    logger_->info("rdma: accepted conn_token={} cm_id={}",
                  conn_token, static_cast<const void*>(id));
  }
}

}  // namespace us3_turbo_access::gateway::data_path::rdma
