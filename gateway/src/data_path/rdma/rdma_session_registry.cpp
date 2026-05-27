#include "data_path/rdma/rdma_session_registry.h"

#include <infiniband/verbs.h>
#include <sys/mman.h>

#include <cstdlib>
#include <utility>

namespace us3_turbo_access::gateway::data_path::rdma {

namespace {

/*
 * 释放 session 级资源（MR + buffer）。连接级资源（PD/CQ/cm_id）不在这里处理，
 * 归 [[rdma_connection_registry]]：保留给后续 PUT 复用。
 * 依赖：MR 引用 PD（PD 归连接），所以先 dereg_mr 再释放 buffer。
 */
void ReleaseSessionResources(RdmaSessionEntry& e) {
  if (e.mr != nullptr) {
    (void)ibv_dereg_mr(e.mr);
    e.mr = nullptr;
  }
  if (e.buffer_data != nullptr && e.buffer_capacity > 0) {
    (void)::munlock(e.buffer_data, e.buffer_capacity);
    std::free(e.buffer_data);
    e.buffer_data = nullptr;
    e.buffer_capacity = 0;
  }
}

}  // namespace

RdmaSessionRegistry::~RdmaSessionRegistry() {
  // 兜底：client 没 Commit 的残留 session（crash / abort 失败）也释放 buffer。
  std::scoped_lock lock(mu_);
  for (auto& kv : entries_) {
    if (kv.second) ReleaseSessionResources(*kv.second);
  }
  entries_.clear();
}

std::shared_ptr<RdmaSessionEntry> RdmaSessionRegistry::CreateForSession(
    std::string session_id, std::string bucket, std::string object_key,
    std::size_t expected_bytes, std::chrono::milliseconds ttl) {
  auto entry = std::make_shared<RdmaSessionEntry>();
  entry->session_id = std::move(session_id);
  entry->bucket = std::move(bucket);
  entry->object_key = std::move(object_key);
  entry->expected_bytes = expected_bytes;
  if (ttl.count() > 0) {
    entry->expire_deadline = std::chrono::steady_clock::now() + ttl;
  }

  std::scoped_lock lock(mu_);
  entries_.emplace(entry->session_id, entry);
  return entry;
}

std::shared_ptr<RdmaSessionEntry> RdmaSessionRegistry::Find(
    std::string_view session_id) {
  std::scoped_lock lock(mu_);
  auto it = entries_.find(std::string(session_id));
  if (it == entries_.end()) return nullptr;
  return it->second;
}

std::vector<std::string> RdmaSessionRegistry::CollectExpired(
    std::chrono::steady_clock::time_point now) const {
  std::vector<std::string> out;
  std::scoped_lock lock(mu_);
  out.reserve(entries_.size());
  for (const auto& kv : entries_) {
    const auto& e = kv.second;
    if (!e) continue;
    // expire_deadline == time_point{} 表示无 TTL，跳过。
    if (e->expire_deadline.time_since_epoch().count() == 0) continue;
    if (e->expire_deadline <= now) out.push_back(kv.first);
  }
  return out;
}

std::uint64_t RdmaSessionRegistry::Erase(std::string_view session_id) {
  std::shared_ptr<RdmaSessionEntry> removed;
  {
    std::scoped_lock lock(mu_);
    auto it = entries_.find(std::string(session_id));
    if (it == entries_.end()) return 0;
    removed = std::move(it->second);
    entries_.erase(it);
  }
  const auto token = removed ? removed->conn_token : 0U;
  ReleaseSessionResources(*removed);
  return token;
}

}  // namespace us3_turbo_access::gateway::data_path::rdma
