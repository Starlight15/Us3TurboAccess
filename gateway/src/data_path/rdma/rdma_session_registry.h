#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ibv_pd;
struct ibv_mr;

namespace us3_turbo_access::gateway::data_path::rdma {

/**
 * 一次 RDMA session 的服务端状态：仅持有 session 级的 buffer/MR；
 * 连接（cm_id/PD/CQ）归 [[rdma_connection_registry]] 管，session 通过
 * conn_token 关联到具体连接。
 *
 * 生命周期：
 *   OpenSession                 → CreateForSession 登记 session_id/bucket/key/size
 *   BindSessionToConnection RPC → 在指定 connection 的 PD 上分配 buffer + reg MR
 *   CommitObject RPC            → 落盘 + ReleaseBuffer（buffer/MR 释放，连接保留）
 */
/**
 * BindSession 的并发状态机：
 *   kUnbound  → kBinding  : 抢到唯一一次绑定权，开始 alloc buffer + reg MR
 *   kBinding  → kBound    : 注册成功，可供 Commit
 *   kBinding  → kUnbound  : 注册失败回滚
 *   kBound    → kBound    : 二次 Bind 视为冲突直接拒绝
 *
 * 用 atomic 而非 mutex：BindSession 路径只在状态切换时短临界区操作，对热路径
 * 友好；具体资源生命周期仍由 RdmaSessionRegistry::Erase 单点回收。
 */
enum class RdmaSessionBindState : std::uint8_t {
  kUnbound = 0,
  kBinding = 1,
  kBound   = 2,
};

struct RdmaSessionEntry {
  std::string         session_id;
  std::string         bucket;
  std::string         object_key;
  std::size_t         expected_bytes{0};
  // 失效时间点：BindSession 超过此值仍未 Commit 视为孤儿，由 sweeper 回收。
  std::chrono::steady_clock::time_point expire_deadline{};

  std::atomic<RdmaSessionBindState>     bind_state{RdmaSessionBindState::kUnbound};

  std::uint64_t       conn_token{0};
  ibv_mr*             mr{nullptr};
  void*               buffer_data{nullptr};
  std::size_t         buffer_capacity{0};
  std::uint64_t       raddr{0};
  std::uint32_t       rkey{0};
};

class RdmaSessionRegistry {
 public:
  RdmaSessionRegistry() = default;
  ~RdmaSessionRegistry();

  /** OpenSession 阶段：仅登记路由信息，buffer/MR 留空待 BindSession。
   *  expire_deadline 由 ttl 与当前时间确定；ttl<=0 时不设过期。*/
  [[nodiscard]] std::shared_ptr<RdmaSessionEntry>
    CreateForSession(std::string session_id, std::string bucket,
                     std::string object_key, std::size_t expected_bytes,
                     std::chrono::milliseconds ttl = std::chrono::milliseconds(0));

  /** Bind / Commit RPC：按 session_id 找 entry。*/
  [[nodiscard]] std::shared_ptr<RdmaSessionEntry>
    Find(std::string_view session_id);

  /** Commit / Abort 后回收 session 级的 buffer + MR（不动连接）。
   *  返回值是 entry 当时绑定的 conn_token（未绑定则为 0），
   *  调用方据此从 ConnectionRegistry::DetachSession。*/
  std::uint64_t Erase(std::string_view session_id);

  /** sweep 用：取出 expire_deadline <= now 的所有 entry id 快照。
   *  调用方拿到列表后逐条调 Erase（+ DetachSession）。*/
  [[nodiscard]] std::vector<std::string>
    CollectExpired(std::chrono::steady_clock::time_point now) const;

 private:
  mutable std::mutex                                                  mu_;
  std::unordered_map<std::string, std::shared_ptr<RdmaSessionEntry>>  entries_;
};

}  // namespace us3_turbo_access::gateway::data_path::rdma
