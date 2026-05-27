#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct rdma_cm_id;
struct ibv_pd;
struct ibv_cq;

namespace us3_turbo_access::gateway::data_path::rdma {

/**
 * 一条已 accept 的 RDMA RC 连接：cm_id + 与 cm_id->verbs 同 context 的 PD/CQ。
 * 资源全部在 CONNECT_REQUEST 时按 cm_id->verbs 现场建出，整条连接生命周期
 * 共生共死；DISCONNECTED 时统一回收。
 *
 * 同一条 connection 上可以承载多个 session 的 buffer/MR：sessions 集合存了
 * 这条连接持有的所有 session_id，连接销毁时通过 on_release hook 反向通知
 * 所属 SessionRegistry 先释放 MR/buffer 再 dealloc PD（否则 MR 会在已失效
 * 的 PD 上 dereg，触发 UB）。
 */
struct RdmaConnectionEntry {
  std::uint64_t                       conn_token{0};
  rdma_cm_id*                         cm_id{nullptr};
  ibv_pd*                             pd{nullptr};
  ibv_cq*                             cq{nullptr};

  std::mutex                          sessions_mu;
  std::unordered_set<std::string>     sessions;  // protected by sessions_mu
};

/**
 * 服务端连接表：CM ACCEPT 时分配 conn_token 写回客户端（private_data），
 * 客户端后续 BindSessionToConnection RPC 用 conn_token 指明在哪条连接的 PD
 * 上注册 buffer。
 *
 * 连接销毁前会通过 set_on_release 注册的 callback 通知上层先释放该连接关联
 * 的所有 session 资源（MR/buffer），再 dealloc PD/cm_id。
 */
class RdmaConnectionRegistry {
 public:
  using OnReleaseFn = std::function<void(const std::vector<std::string>&)>;

  RdmaConnectionRegistry() = default;
  ~RdmaConnectionRegistry();

  RdmaConnectionRegistry(const RdmaConnectionRegistry&) = delete;
  RdmaConnectionRegistry& operator=(const RdmaConnectionRegistry&) = delete;

  /** 由 RdmaExecutor 在 Start 阶段注入；EraseByCmId 销毁连接前回调以释放挂在
   *  该连接上的所有 session（按 session_id 列表，调用方负责按 id 释放对应资源）。*/
  void set_on_release(OnReleaseFn fn);

  /** CM ACCEPT 阶段调；返回新分配的 conn_token。资源 owner 转给 registry。*/
  [[nodiscard]] std::uint64_t
    Register(rdma_cm_id* cm_id, ibv_pd* pd, ibv_cq* cq);

  /** Bind / Commit RPC 用：按 conn_token 找连接。*/
  [[nodiscard]] std::shared_ptr<RdmaConnectionEntry>
    Find(std::uint64_t conn_token);

  /** BindSession 成功时调，把 session_id 挂到该连接上。*/
  bool AttachSession(std::uint64_t conn_token, const std::string& session_id);

  /** Commit / Abort 后调，把 session_id 从连接上摘掉（连接本身保留）。*/
  void DetachSession(std::uint64_t conn_token, const std::string& session_id);

  /** DISCONNECTED 事件时调；先调 on_release 释放 sessions，再销毁 QP/CQ/PD/cm_id。*/
  bool EraseByCmId(rdma_cm_id* cm_id);

 private:
  std::mutex                                                       mu_;
  std::unordered_map<std::uint64_t, std::shared_ptr<RdmaConnectionEntry>> by_token_;
  std::unordered_map<rdma_cm_id*, std::uint64_t>                   cmid_to_token_;
  std::uint64_t                                                    next_token_{1};
  OnReleaseFn                                                      on_release_;
};

}  // namespace us3_turbo_access::gateway::data_path::rdma
