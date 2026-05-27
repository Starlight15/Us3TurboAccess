#include "client/src/transports/rdma/rdma_completion_driver.h"

#include <infiniband/verbs.h>

#include <chrono>
#include <thread>
#include <utility>

namespace us3_turbo_access::client {

RdmaCompletionDriver::RdmaCompletionDriver() = default;
RdmaCompletionDriver::~RdmaCompletionDriver() { Stop(); }

void RdmaCompletionDriver::Start() {
  std::scoped_lock lock(mu_);
  if (started_) return;
  stop_.store(false, std::memory_order_release);
  thread_ = std::thread(&RdmaCompletionDriver::Loop, this);
  started_ = true;
}

void RdmaCompletionDriver::Stop() {
  {
    std::scoped_lock lock(mu_);
    if (!started_) return;
  }
  stop_.store(true, std::memory_order_release);
  if (thread_.joinable()) thread_.join();
  // join 之后 driver 已经不会再 set_value 任何 promise；剩下的全部 promise
  // 必须由 Stop 显式 set_exception，否则上层 fut.get() 永远悬挂。
  std::scoped_lock lock(mu_);
  for (auto& kv : conns_) {
    for (auto& iv : kv.second.inflight) {
      try {
        iv.second.set_exception(std::make_exception_ptr(
            std::runtime_error("RdmaCompletionDriver stopped")));
      } catch (...) {}
    }
    kv.second.inflight.clear();
  }
  conns_.clear();
  started_ = false;
}

// 用 conn_id（单调递增的内部 ID）作 Inflight 索引，而非 ibv_cq* 指针：
// 同一 ibv_cq* 可能在 destroy 后被 librdmacm 重新分配出同址给新连接，
// 拿指针做 key 会出现 ABA。
std::uint64_t RdmaCompletionDriver::Attach(ibv_cq* cq) {
  std::scoped_lock lock(mu_);
  const auto id = next_conn_id_++;
  ConnSlot s;
  s.cq = cq;
  conns_.emplace(id, std::move(s));
  return id;
}

/*
 * Detach 必须保证返回后 driver 不再触摸该 CQ，否则调用方紧接着 ibv_destroy_cq
 * 会撞 driver 上一轮 snapshot 仍持有的指针。两步：
 *   1. 持锁删 map（新一轮 snapshot 不再含本 conn_id）；
 *   2. 等 epoch 至少前进 2 —— driver 在第 1 步前可能已经 release 锁开始 poll，
 *      epoch 前进 1 表示当前轮跑完（snapshot 已 drain），前进 2 表示完整下一轮
 *      已经开始且只能基于新 map，可安全销毁该 CQ。
 * 残留 inflight set_exception，避免 future 永远悬挂。
 */
void RdmaCompletionDriver::Detach(std::uint64_t conn_id) {
  std::unordered_map<std::uint64_t, std::promise<RdmaCompletion>> to_drain;
  bool was_running = false;
  std::uint64_t snapshot_epoch = 0;
  {
    std::scoped_lock lock(mu_);
    auto it = conns_.find(conn_id);
    if (it == conns_.end()) return;
    to_drain = std::move(it->second.inflight);
    conns_.erase(it);
    was_running = started_ && !stop_.load(std::memory_order_acquire);
    snapshot_epoch = epoch_.load(std::memory_order_acquire);
  }
  for (auto& iv : to_drain) {
    try {
      iv.second.set_exception(std::make_exception_ptr(
          std::runtime_error("RDMA connection detached before completion")));
    } catch (...) {}
  }
  if (was_running) {
    WaitEpochAdvance(snapshot_epoch + 2);
  }
}

// Stop 路径也唤醒等待者（Loop 退出前会 fetch_add + notify_all），否则在 driver
// 已停的情况下 Detach 会永久挂在 cv.wait 上。
void RdmaCompletionDriver::WaitEpochAdvance(std::uint64_t at_least) {
  std::unique_lock<std::mutex> lk(mu_);
  epoch_cv_.wait(lk, [this, at_least] {
    return stop_.load(std::memory_order_acquire) ||
           epoch_.load(std::memory_order_acquire) >= at_least;
  });
}

// 调用方必须在 PostWrite 之前调本函数登记 promise，否则 driver 收到 WC 时
// 找不到目标，整批 WC 静默丢弃，fut.get() 永远悬挂。
std::future<RdmaCompletion>
RdmaCompletionDriver::RegisterInflight(std::uint64_t conn_id,
                                        std::uint64_t wr_id) {
  std::promise<RdmaCompletion> p;
  auto fut = p.get_future();
  std::scoped_lock lock(mu_);
  auto it = conns_.find(conn_id);
  if (it == conns_.end()) {
    // 连接已 Detach（或从未 Attach），让 future 立刻抛而不是吞掉。
    p.set_exception(std::make_exception_ptr(
        std::runtime_error("RegisterInflight on unknown conn_id")));
    return fut;
  }
  it->second.inflight.emplace(wr_id, std::move(p));
  return fut;
}

// 主循环：对所有 attach 的 CQ 做 round-robin poll。先快照锁外 poll、再持锁
// 把 WC 派发给对应 promise——避免 ibv_poll_cq（kernel call，毫秒级）持锁阻塞
// 其它线程的 Register/Detach。空转时 50us sleep 防止 100% 烧 CPU。
//
// 每跑完一轮 epoch_ +1 并 notify_all：Detach 正是靠"epoch 前进 ≥2"判定 driver
// 已经丢弃了它上一轮快照中的 cq 指针，从而安全销毁该 CQ。
void RdmaCompletionDriver::Loop() {
  ibv_wc wcs[16];
  while (!stop_.load(std::memory_order_acquire)) {
    bool any = false;
    // 1) 持锁拍快照：本轮要 poll 的 (conn_id, cq) 列表。
    std::vector<std::pair<std::uint64_t, ibv_cq*>> snapshot;
    {
      std::scoped_lock lock(mu_);
      snapshot.reserve(conns_.size());
      for (const auto& kv : conns_) {
        snapshot.emplace_back(kv.first, kv.second.cq);
      }
    }
    // 2) 锁外 poll：每个 cq 一次最多取 16 条 WC，按 conn_id 派发到 promise。
    for (auto& [cid, cq] : snapshot) {
      if (cq == nullptr) continue;
      const int n = ibv_poll_cq(cq, 16, wcs);
      if (n <= 0) continue;
      any = true;
      std::scoped_lock lock(mu_);
      auto it = conns_.find(cid);
      // 本轮 poll 跟 Detach 并发时可能丢失：连接已被摘掉，本批 WC 直接抛弃。
      // 上层会通过 promise 的 set_exception 拿到失败（在 Detach 路径里）。
      if (it == conns_.end()) continue;
      auto& slot = it->second;
      for (int i = 0; i < n; ++i) {
        auto iv = slot.inflight.find(wcs[i].wr_id);
        // 未登记的 wr_id 静默丢弃：可能是已抢答的请求或外部测试桩。
        if (iv == slot.inflight.end()) continue;
        RdmaCompletion comp;
        comp.status = static_cast<int>(wcs[i].status);
        comp.wr_id  = wcs[i].wr_id;
        try { iv->second.set_value(comp); } catch (...) {}
        slot.inflight.erase(iv);
      }
    }
    if (!any) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    // 3) 一轮结束：epoch +1 让 Detach 等待者推进。
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    epoch_cv_.notify_all();
  }
  // Stop：再 +1 + notify_all 一次，唤醒可能仍在 WaitEpochAdvance 的 Detach
  // （它们看到 stop_==true 也会立刻退出 wait）。
  epoch_.fetch_add(1, std::memory_order_acq_rel);
  epoch_cv_.notify_all();
}

}  // namespace us3_turbo_access::client
