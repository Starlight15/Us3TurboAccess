#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <spdlog/logger.h>

#include "core/session/session.h"
#include "us3_turbo_access/gateway/result.h"

namespace us3_turbo_access::gateway::core {

/**
 * @brief In-memory registry of active transfer sessions, sharded for
 *        concurrency.
 *
 * 内部按 64 个 shard 切分，每个 shard 维护 (by_id, by_ticket, by_idempotency)
 * 三张本地索引 + 一把 std::mutex。同一个 Session 的副本可能同时出现在不同
 * shard 上（按 session_id / ticket / idempotency_key 各自哈希）；shared_ptr
 * 兜底引用计数，sweep / expire 时一并清理。
 */
class SessionStore {
 public:
  SessionStore(std::string gateway_id, std::chrono::seconds session_ttl,
               std::shared_ptr<spdlog::logger> logger);

  [[nodiscard]] Result<std::shared_ptr<Session>> Create(const OpenSessionParams& req);

  [[nodiscard]] Result<std::shared_ptr<Session>>
    Find(std::string_view session_id) const;

  [[nodiscard]] Result<std::shared_ptr<Session>>
    FindByTicket(std::string_view ticket) const;

  [[nodiscard]] Result<std::shared_ptr<Session>>
    Claim(std::string_view ticket);

  [[nodiscard]] Result<std::shared_ptr<Session>>
    MarkActive(std::string_view session_id);

  /**
   * @brief Idempotent activation: CAS kOpened→kActive; no-op if already active.
   *        Returns the session if found, even when no transition happened.
   */
  [[nodiscard]] Result<std::shared_ptr<Session>>
    BumpActive(std::string_view session_id);

  [[nodiscard]] Result<std::shared_ptr<Session>>
    MarkCompleted(std::string_view session_id);

  [[nodiscard]] Result<std::shared_ptr<Session>>
    MarkFailed(std::string_view session_id);

  std::size_t SweepExpired(std::chrono::steady_clock::time_point now);

 private:
  static constexpr std::size_t kShardCount = 64;

  struct Shard {
    mutable std::mutex                                          mu;
    std::unordered_map<std::string, std::shared_ptr<Session>>   by_id;
    std::unordered_map<std::string, std::shared_ptr<Session>>   by_ticket;
    std::unordered_map<std::string, std::shared_ptr<Session>>   by_idempotency;
  };

  Result<std::shared_ptr<Session>>
    Transition(std::string_view session_id, SessionState expected,
               SessionState desired, ErrorCode failure_code);

  [[nodiscard]] Shard& ShardFor(std::string_view key) const;

  std::shared_ptr<spdlog::logger>     logger_;
  std::string                         gateway_id_;
  std::chrono::seconds                session_ttl_{0};

  mutable std::array<Shard, kShardCount> shards_{};
};

}  // namespace us3_turbo_access::gateway::core
