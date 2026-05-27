#include "data_path/rdma/rdma_connection_registry.h"

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include <utility>
#include <vector>

namespace us3_turbo_access::gateway::data_path::rdma {

namespace {

/*
 * 释放 entry 上挂的连接级资源。依赖链：
 *   QP 引用 PD/CQ；CQ/PD 之间无依赖；cm_id 与上面 verbs 句柄独立。
 * 销毁顺序：QP → CQ → PD → cm_id。session 级的 buffer/MR 已由 on_release
 * 回调先行清理（必须在 dealloc PD 之前完成）。
 */
void ReleaseConnection(RdmaConnectionEntry& e) {
  if (e.cm_id != nullptr && e.cm_id->qp != nullptr) {
    rdma_destroy_qp(e.cm_id);
  }
  if (e.cq != nullptr) {
    (void)ibv_destroy_cq(e.cq);
    e.cq = nullptr;
  }
  if (e.pd != nullptr) {
    (void)ibv_dealloc_pd(e.pd);
    e.pd = nullptr;
  }
  if (e.cm_id != nullptr) {
    rdma_destroy_id(e.cm_id);
    e.cm_id = nullptr;
  }
}

}  // namespace

RdmaConnectionRegistry::~RdmaConnectionRegistry() {
  // 进程退出兜底：先收集所有 entry 的 sessions 全量回调一次（让 SessionRegistry
  // 释放 MR/buffer），再依次销毁连接级资源。
  std::vector<std::shared_ptr<RdmaConnectionEntry>> drained;
  OnReleaseFn cb;
  {
    std::scoped_lock lock(mu_);
    drained.reserve(by_token_.size());
    for (auto& kv : by_token_) {
      if (kv.second) drained.push_back(std::move(kv.second));
    }
    by_token_.clear();
    cmid_to_token_.clear();
    cb = on_release_;  // 拷一份释放锁后用
  }
  for (auto& e : drained) {
    if (cb) {
      std::vector<std::string> snapshot;
      {
        std::scoped_lock lk(e->sessions_mu);
        snapshot.assign(e->sessions.begin(), e->sessions.end());
        e->sessions.clear();
      }
      if (!snapshot.empty()) cb(snapshot);
    }
    ReleaseConnection(*e);
  }
}

void RdmaConnectionRegistry::set_on_release(OnReleaseFn fn) {
  std::scoped_lock lock(mu_);
  on_release_ = std::move(fn);
}

std::uint64_t RdmaConnectionRegistry::Register(rdma_cm_id* cm_id, ibv_pd* pd,
                                                  ibv_cq* cq) {
  auto entry = std::make_shared<RdmaConnectionEntry>();
  entry->cm_id = cm_id;
  entry->pd    = pd;
  entry->cq    = cq;
  std::scoped_lock lock(mu_);
  const auto token = next_token_++;
  entry->conn_token = token;
  by_token_.emplace(token, entry);
  cmid_to_token_.emplace(cm_id, token);
  return token;
}

std::shared_ptr<RdmaConnectionEntry> RdmaConnectionRegistry::Find(
    std::uint64_t conn_token) {
  std::scoped_lock lock(mu_);
  auto it = by_token_.find(conn_token);
  if (it == by_token_.end()) return nullptr;
  return it->second;
}

bool RdmaConnectionRegistry::AttachSession(std::uint64_t conn_token,
                                             const std::string& session_id) {
  auto entry = Find(conn_token);
  if (entry == nullptr) return false;
  std::scoped_lock lk(entry->sessions_mu);
  entry->sessions.insert(session_id);
  return true;
}

void RdmaConnectionRegistry::DetachSession(std::uint64_t conn_token,
                                            const std::string& session_id) {
  auto entry = Find(conn_token);
  if (entry == nullptr) return;
  std::scoped_lock lk(entry->sessions_mu);
  entry->sessions.erase(session_id);
}

bool RdmaConnectionRegistry::EraseByCmId(rdma_cm_id* cm_id) {
  std::shared_ptr<RdmaConnectionEntry> removed;
  OnReleaseFn cb;
  {
    std::scoped_lock lock(mu_);
    auto it = cmid_to_token_.find(cm_id);
    if (it == cmid_to_token_.end()) return false;
    const auto token = it->second;
    cmid_to_token_.erase(it);
    auto eit = by_token_.find(token);
    if (eit != by_token_.end()) {
      removed = std::move(eit->second);
      by_token_.erase(eit);
    }
    cb = on_release_;
  }
  if (removed) {
    // 1) 先让上层把挂在这条连接上的 sessions 释放（dereg MR + free buffer）。
    if (cb) {
      std::vector<std::string> snapshot;
      {
        std::scoped_lock lk(removed->sessions_mu);
        snapshot.assign(removed->sessions.begin(), removed->sessions.end());
        removed->sessions.clear();
      }
      if (!snapshot.empty()) cb(snapshot);
    }
    // 2) 再销毁连接级资源（QP → CQ → PD → cm_id）。
    ReleaseConnection(*removed);
  }
  return true;
}

}  // namespace us3_turbo_access::gateway::data_path::rdma
