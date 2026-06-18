#include "proxy/src/session/session_manager.h"

#include <chrono>
#include <ctime>
#include <string>
#include <utility>

namespace us3_turbo_access::proxy {

namespace {

// session_id 前缀，与 OpenSession 历史实现一致。
constexpr std::string_view kSessionIdPrefix = "pxs-";

}  // namespace

SessionManager::SessionManager(std::int64_t ttl_sec) : ttl_sec_(ttl_sec) {}

SessionManager::Session SessionManager::CreateSession(
    const std::string& session_id_hint) {
  // ---- 生成 session_id ----
  std::string session_id;
  if (!session_id_hint.empty()) {
    session_id = session_id_hint;
  } else {
    const auto seq = seq_.fetch_add(1, std::memory_order_relaxed);
    session_id = std::string(kSessionIdPrefix) + std::to_string(seq);
  }

  // ---- 生成 ticket ----
  const std::string ticket = std::string("tkt-") + session_id;

  // ---- expire_at ----
  const auto expire_time =
      std::chrono::system_clock::now() + std::chrono::seconds(ttl_sec_);
  const auto expire_at_time_t =
      std::chrono::system_clock::to_time_t(expire_time);
  // ISO 8601 近似格式，与 gateway SessionOpener 风格一致。
  char expire_buf[32] = {};
  std::strftime(expire_buf, sizeof(expire_buf), "%Y-%m-%dT%H:%M:%SZ",
                std::gmtime(&expire_at_time_t));

  return Session{std::move(session_id), std::move(ticket), expire_buf};
}

}  // namespace us3_turbo_access::proxy
