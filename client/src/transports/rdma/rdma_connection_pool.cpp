#include "client/src/transports/rdma/rdma_connection_pool.h"

#include <utility>

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

ConnectionHandle::ConnectionHandle(RdmaConnectionPool* pool,
                                    std::unique_ptr<PooledConnection> entry) noexcept
    : pool_(pool), entry_(std::move(entry)) {}

ConnectionHandle::ConnectionHandle(ConnectionHandle&& o) noexcept
    : pool_(o.pool_), entry_(std::move(o.entry_)) {
  o.pool_ = nullptr;
}

// move 赋值：先把现有 entry 归还，再接管 o 的状态。否则会泄漏当前 entry，
// 连接永远回不到池子。
ConnectionHandle& ConnectionHandle::operator=(ConnectionHandle&& o) noexcept {
  if (this != &o) {
    if (entry_ != nullptr && pool_ != nullptr) {
      pool_->ReturnOrDiscard(std::move(entry_));
    }
    pool_  = o.pool_;
    entry_ = std::move(o.entry_);
    o.pool_ = nullptr;
  }
  return *this;
}

// 析构默认走 ReturnOrDiscard：成功路径自然归池复用。
// 调用方在错误路径上要显式 Discard()，否则可能把脏连接放回池子。
ConnectionHandle::~ConnectionHandle() {
  if (entry_ != nullptr && pool_ != nullptr) {
    pool_->ReturnOrDiscard(std::move(entry_));
  }
}

// 显式丢弃，置空 entry_ 之后析构不会再触发 ReturnOrDiscard。
void ConnectionHandle::Discard() noexcept {
  if (entry_ == nullptr) return;
  if (pool_ != nullptr) pool_->DiscardOne(std::move(entry_));
  pool_ = nullptr;
}

RdmaConnectionPool::RdmaConnectionPool(const RdmaClientOptions& opts,
                                        RdmaCompletionDriver& driver)
    : opts_(opts), driver_(driver) {}

RdmaConnectionPool::~RdmaConnectionPool() { Shutdown(); }

void RdmaConnectionPool::Shutdown() {
  std::unordered_map<std::string,
                     std::deque<std::unique_ptr<PooledConnection>>> snapshot;
  {
    std::scoped_lock lock(mu_);
    if (shutdown_) return;
    shutdown_ = true;
    snapshot = std::move(idle_);
    idle_.clear();
  }
  // 释放锁后逐条销毁：先 Detach（同步等 driver 跨过下一轮，避免 driver 仍持有
  // 这条 CQ 的指针），再 reset conn 触发 RdmaCmConnection::Disconnect。
  for (auto& kv : snapshot) {
    for (auto& entry : kv.second) {
      driver_.Detach(entry->driver_conn_id);
      entry->conn.reset();
    }
  }
}

// 池 hit 走快速路径；miss 时锁外做 CM 三次握手（毫秒级，不能持锁阻塞别人）。
Result<ConnectionHandle> RdmaConnectionPool::Acquire(const std::string& host,
                                                       std::uint16_t port) {
  const std::string key = host + ":" + std::to_string(port);
  // 快路径：拿一条空闲，O(1)。
  {
    std::scoped_lock lock(mu_);
    if (shutdown_) {
      return Result<ConnectionHandle>::Failure(MakeTransportFailure(
          "RdmaConnectionPool already shut down",
          DataPath::kNativeRdma, "", false));
    }
    auto it = idle_.find(key);
    if (it != idle_.end() && !it->second.empty()) {
      auto entry = std::move(it->second.front());
      it->second.pop_front();
      acquire_hit_.fetch_add(1, std::memory_order_relaxed);
      return Result<ConnectionHandle>::Success(
          ConnectionHandle(this, std::move(entry)));
    }
  }

  // 慢路径：锁外 CM 握手；Attach 必须在 Connect 拿到 cq 之后。
  // 注：这里不预先 reserve idle_[key]，新连接首次归还时再分配 deque。
  acquire_miss_.fetch_add(1, std::memory_order_relaxed);
  auto conn = std::make_unique<RdmaCmConnection>(opts_);
  auto connected = conn->Connect(host, port);
  if (!connected.success()) {
    return Result<ConnectionHandle>::Failure(connected.error());
  }
  auto driver_id = driver_.Attach(conn->cq());

  auto entry            = std::make_unique<PooledConnection>();
  entry->conn           = std::move(conn);
  entry->driver_conn_id = driver_id;
  entry->endpoint       = std::move(key);
  return Result<ConnectionHandle>::Success(
      ConnectionHandle(this, std::move(entry)));
}

// 归还策略：上限以内放回 free list 复用；超过上限直接 Discard，避免单个端点
// idle 连接无限堆积占资源。shutdown 之后全部 Discard。
void RdmaConnectionPool::ReturnOrDiscard(
    std::unique_ptr<PooledConnection> entry) noexcept {
  if (entry == nullptr) return;
  {
    std::scoped_lock lock(mu_);
    if (!shutdown_) {
      auto& q = idle_[entry->endpoint];
      if (q.size() < opts_.pool_max_idle_per_endpoint) {
        q.push_back(std::move(entry));
        return_kept_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }
  // 锁外销毁：DiscardOne 会调 driver_.Detach（同步等 driver epoch+2），
  // 持池锁会跟 driver Loop 抢锁，可能让其它 Acquire 短暂卡顿。
  return_dropped_.fetch_add(1, std::memory_order_relaxed);
  DiscardOne(std::move(entry));
}

void RdmaConnectionPool::DiscardOne(
    std::unique_ptr<PooledConnection> entry) noexcept {
  if (entry == nullptr) return;
  // 销毁顺序：先 Detach（driver 跨过下一轮 polling 不再触摸 CQ）→ 再 reset conn
  // 触发 RdmaCmConnection::Disconnect 销毁 QP/CQ/PD。
  driver_.Detach(entry->driver_conn_id);
  entry->conn.reset();
}

RdmaConnectionPool::PoolStats RdmaConnectionPool::stats() const noexcept {
  PoolStats s{};
  s.acquire_hit    = acquire_hit_.load(std::memory_order_relaxed);
  s.acquire_miss   = acquire_miss_.load(std::memory_order_relaxed);
  s.return_kept    = return_kept_.load(std::memory_order_relaxed);
  s.return_dropped = return_dropped_.load(std::memory_order_relaxed);
  // idle_size 需要锁；这里短锁就拿 size，再立刻释放
  std::size_t idle_total = 0;
  {
    std::scoped_lock lock(const_cast<std::mutex&>(mu_));
    for (const auto& kv : idle_) idle_total += kv.second.size();
  }
  s.idle_size = idle_total;
  return s;
}

}  // namespace us3_turbo_access::client
