#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "client/src/transports/rdma/rdma_cm_connection.h"
#include "client/src/transports/rdma/rdma_completion_driver.h"
#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

class RdmaConnectionPool;

/**
 * 一条池子里活跃的 RDMA 连接：cm_id/PD/CQ 由 RdmaCmConnection 持有，
 * driver_conn_id 关联到 RdmaCompletionDriver 上的 CQ slot。
 *
 * 用 ConnectionHandle 借出/归还；不在池子的状态下也可以直接当连接用
 * （例如 pool.Acquire 失败时降级到一次性连接）。
 */
struct PooledConnection {
  std::unique_ptr<RdmaCmConnection> conn;
  std::uint64_t                     driver_conn_id{0};
  std::string                       endpoint;  // host:port

  // per-连接 wr_id 计数器。WR completion 通过 (driver_conn_id, wr_id) 路由
  // 回 promise，所以 wr_id 只要在同一 QP 内唯一即可；用全局原子既是过度
  // 同步、也错误暗示了跨连接关联。
  std::atomic<std::uint64_t>        next_wr_id{1};
};

/**
 * RAII handle：作用域结束时把连接归还池子（或丢弃）。
 * !valid() 表示借不出连接；调用方应直接拿 raw error 返回。
 */
class ConnectionHandle {
 public:
  ConnectionHandle() = default;
  ConnectionHandle(RdmaConnectionPool* pool,
                   std::unique_ptr<PooledConnection> entry) noexcept;
  ~ConnectionHandle();

  ConnectionHandle(const ConnectionHandle&) = delete;
  ConnectionHandle& operator=(const ConnectionHandle&) = delete;
  ConnectionHandle(ConnectionHandle&& o) noexcept;
  ConnectionHandle& operator=(ConnectionHandle&& o) noexcept;

  [[nodiscard]] bool                valid() const noexcept { return entry_ != nullptr; }
  [[nodiscard]] RdmaCmConnection*   conn() const noexcept {
    return entry_ != nullptr ? entry_->conn.get() : nullptr;
  }
  [[nodiscard]] std::uint64_t       driver_conn_id() const noexcept {
    return entry_ != nullptr ? entry_->driver_conn_id : 0U;
  }

  /** 取下一个 wr_id（per-连接单调递增），未持有连接时返回 0。*/
  [[nodiscard]] std::uint64_t       next_wr_id() noexcept {
    return entry_ != nullptr
               ? entry_->next_wr_id.fetch_add(1, std::memory_order_relaxed)
               : 0U;
  }

  /** 显式丢弃连接（出错时调；连接析构 + driver detach，不回池子）。*/
  void Discard() noexcept;

 private:
  RdmaConnectionPool*               pool_{nullptr};
  std::unique_ptr<PooledConnection> entry_;
};

/**
 * 客户端 RDMA QP 连接池。按 (host:port) 维护 free list，超出
 * RdmaClientOptions::pool_max_idle_per_endpoint 的连接会被丢弃。
 *
 * 析构时关闭所有缓存的连接 + Detach driver；调用方需保证 ConnectionHandle
 * 已归还。
 */
class RdmaConnectionPool {
 public:
  explicit RdmaConnectionPool(const RdmaClientOptions& opts,
                              RdmaCompletionDriver& driver);
  ~RdmaConnectionPool();

  RdmaConnectionPool(const RdmaConnectionPool&) = delete;
  RdmaConnectionPool& operator=(const RdmaConnectionPool&) = delete;

  /**
   * 借一条到 (host, port) 的 RC 连接。池子里有空闲就直接拿；否则现场 CM connect。
   * 返回的 handle 在析构时自动归还。
   */
  [[nodiscard]] Result<ConnectionHandle>
    Acquire(const std::string& host, std::uint16_t port);

  /** 关闭所有缓存连接。Pool 析构时也会调。*/
  void Shutdown();

  /**
   * R.5 复用诊断计数（atomic，无锁读）。每个 RDMA 客户端进程一个 pool 实例；
   * 多 client 时按实例独立。bench 跑完读 stats() 看命中率。
   *   acquire_hit  : Acquire 在 idle_ 找到现成连接，省去 CM 三次握手
   *   acquire_miss : Acquire 池空，现场建新连接（含 CM）
   *   return_kept  : ReturnOrDiscard 归回池子复用
   *   return_dropped: 超 pool_max_idle 丢弃 / Discard() 显式丢弃
   * 期望：稳态下 acquire_hit / total ≈ 1，return_dropped ≈ 0。
   */
  struct PoolStats {
    std::int64_t acquire_hit;
    std::int64_t acquire_miss;
    std::int64_t return_kept;
    std::int64_t return_dropped;
    std::size_t  idle_size;  // 所有 endpoint 的 idle 总数
  };
  [[nodiscard]] PoolStats stats() const noexcept;

  // ConnectionHandle 析构时通过 friend 调用，归还连接到池子或者丢弃。
  friend class ConnectionHandle;

 private:
  void ReturnOrDiscard(std::unique_ptr<PooledConnection> entry) noexcept;
  void DiscardOne(std::unique_ptr<PooledConnection> entry) noexcept;

  RdmaClientOptions                                                opts_;
  RdmaCompletionDriver&                                            driver_;

  std::mutex                                                       mu_;
  std::unordered_map<std::string,
                     std::deque<std::unique_ptr<PooledConnection>>> idle_;
  bool                                                             shutdown_{false};

  // R.5 计数（atomic，不持锁）
  std::atomic<std::int64_t>                                        acquire_hit_{0};
  std::atomic<std::int64_t>                                        acquire_miss_{0};
  std::atomic<std::int64_t>                                        return_kept_{0};
  std::atomic<std::int64_t>                                        return_dropped_{0};
};

}  // namespace us3_turbo_access::client
