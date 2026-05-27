#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

struct rdma_cm_id;
struct rdma_event_channel;
struct ibv_qp;
struct ibv_pd;
struct ibv_cq;

namespace us3_turbo_access::client {

/**
 * 客户端 RDMA CM 主动方连接。一次连接对应一条 RC QP。
 *
 * PD/CQ 由本对象在 resolve_route 后基于 cm_id->verbs 自建，确保与 rdma_cm
 * 路由选中的 device context 严格匹配（用全局共享 PD 会因 device 不匹配
 * 而触发 rdma_create_qp failed）。
 *
 * 同步阻塞接口：Connect 等到 ESTABLISHED 才返回。
 */
class RdmaCmConnection {
 public:
  explicit RdmaCmConnection(const RdmaClientOptions& opts);
  ~RdmaCmConnection();

  RdmaCmConnection(const RdmaCmConnection&) = delete;
  RdmaCmConnection& operator=(const RdmaCmConnection&) = delete;

  /**
   * 连接 gateway。CONNECT 不携带 session 信息（连接与 session 解耦）；
   * 服务端 ACCEPT 阶段通过 private_data 回传 conn_token，后续 RPC 用 conn_token
   * 指明在哪条连接上 bind session。
   */
  [[nodiscard]] Result<bool>
    Connect(const std::string& host, std::uint16_t port);

  void Disconnect();

  [[nodiscard]] rdma_cm_id* cm_id() const noexcept { return cm_id_; }
  [[nodiscard]] ibv_qp*     qp()    const noexcept;
  [[nodiscard]] ibv_pd*     pd()    const noexcept { return pd_; }
  [[nodiscard]] ibv_cq*     cq()    const noexcept { return cq_; }

  /** Connect 成功后可用：服务端通过 CM ACCEPT private_data 回传的 conn_token。*/
  [[nodiscard]] std::uint64_t conn_token() const noexcept { return conn_token_; }

  /** 上次 BindSessionToConnection RPC 写进来的远端 buffer 地址（RAII 由调用方维护）。*/
  void set_remote_buffer(std::uint64_t raddr, std::uint32_t rkey) noexcept {
    remote_raddr_ = raddr;
    remote_rkey_  = rkey;
  }
  [[nodiscard]] std::uint64_t remote_raddr() const noexcept { return remote_raddr_; }
  [[nodiscard]] std::uint32_t remote_rkey()  const noexcept { return remote_rkey_; }

 private:
  RdmaClientOptions               opts_;
  rdma_event_channel*             event_channel_{nullptr};
  rdma_cm_id*                     cm_id_{nullptr};
  ibv_pd*                         pd_{nullptr};
  ibv_cq*                         cq_{nullptr};
  std::uint64_t                   conn_token_{0};
  std::uint64_t                   remote_raddr_{0};
  std::uint32_t                   remote_rkey_{0};
  bool                            connected_{false};
};

}  // namespace us3_turbo_access::client
