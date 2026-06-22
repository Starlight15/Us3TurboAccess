#include "data_path/ucx/ucx_session_registry.h"

namespace us3_turbo_access::gateway::data_flow::ucx {

std::shared_ptr<UcxSessionEntry> UcxSessionRegistry::Create(
    std::string session_id, std::string bucket,
    std::string object_key, std::size_t expected_bytes) {
  // 所有字段在锁外构造完成，entry 完整后才插入 map，消除并发 Find 读到半初始化状态
  auto entry = std::make_shared<UcxSessionEntry>();
  entry->session_id     = session_id;
  entry->bucket         = std::move(bucket);
  entry->object_key     = std::move(object_key);
  entry->expected_bytes = expected_bytes;

  std::lock_guard<std::mutex> lock(mu_);
  entries_[std::move(session_id)] = entry;
  return entry;
}

std::shared_ptr<UcxSessionEntry>
UcxSessionRegistry::Find(std::string_view session_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = entries_.find(std::string(session_id));
  return it != entries_.end() ? it->second : nullptr;
}

std::shared_ptr<UcxSessionEntry>
UcxSessionRegistry::Erase(std::string_view session_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = entries_.find(std::string(session_id));
  if (it == entries_.end()) return nullptr;
  auto entry = std::move(it->second);
  entries_.erase(it);
  return entry;
}

}  // namespace us3_turbo_access::gateway::data_flow::ucx
