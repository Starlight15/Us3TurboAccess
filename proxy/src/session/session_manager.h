#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace us3_turbo_access::proxy {

/**
 * @brief 生成并持有 proxy 控制面的 session 凭证。
 *
 * 职责：
 * - 生成 session_id（格式 "pxs-<seq>"）
 * - 生成 ticket（格式 "tkt-<session_id>"）
 * - 计算 expire_at（ISO 8601，如 "2026-06-18T09:00:00Z"）
 *
 * 不持有 session 索引：v1 由 client 持有 ticket，proxy 无状态，
 * 本类只负责生成凭证，不负责校验/过期清理（留待后续）。
 *
 * 线程安全：seq_ 为 std::atomic，CreateSession 无锁可并发调用。
 *
 * 依赖：纯标准库，不依赖 brpc（可单元测试）。
 */
class SessionManager {
 public:
  struct Session {
    std::string session_id;  // "pxs-<seq>"
    std::string ticket;      // "tkt-<session_id>"
    std::string expire_at;   // ISO 8601，如 "2026-06-18T09:00:00Z"
  };

  // ttl_sec: session 有效期（秒），用于计算 expire_at。
  explicit SessionManager(std::int64_t ttl_sec);

  // 生成新 session。session_id_hint 非空时直接用作 session_id
  // （来自 request->session_id()，幂等场景）；为空则自增分配 "pxs-<seq>"。
  Session CreateSession(const std::string& session_id_hint = "");

 private:
  std::int64_t ttl_sec_;
  std::atomic<std::uint64_t> seq_{0};
};

}  // namespace us3_turbo_access::proxy
