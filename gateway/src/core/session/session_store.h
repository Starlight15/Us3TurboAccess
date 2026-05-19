#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <spdlog/logger.h>

#include "core/session/rdma_parameters.h"
#include "core/session/session.h"
#include "us3_turbo_access/gateway/result.h"

namespace us3_turbo_access::gateway::core {

/**
 * @brief In-memory registry of active transfer sessions.
 *
 * Holds session metadata, ticket lookup, and basic state-machine transitions.
 * One `std::mutex` protects all three index maps; this is enough for the M1
 * scale target (≤ tens of thousands of concurrent sessions). Sharding is
 * deferred until contention is observed.
 */
class SessionStore {
 public:
  SessionStore(std::string gateway_id, std::string gateway_endpoint,
               std::size_t default_chunk_size, std::chrono::seconds session_ttl,
               std::shared_ptr<spdlog::logger> logger);

  /**
   * @brief Creates and registers a new session.
   *
   * Honours @p req.idempotency_key when supplied. When a previous session
   * already exists for the same key, that record is returned unchanged.
   */
  [[nodiscard]] Result<std::shared_ptr<Session>> Create(const NegotiateRequest& req);

  [[nodiscard]] Result<std::shared_ptr<Session>>
    Find(std::string_view session_id) const;

  [[nodiscard]] Result<std::shared_ptr<Session>>
    FindByTicket(std::string_view ticket) const;

  /**
   * @brief Atomically transitions a session into kClaimed.
   *
   * Returns kStaleState when the session is not in a claimable state, and
   * kTicketInvalid when the ticket does not resolve to a known session.
   */
  [[nodiscard]] Result<std::shared_ptr<Session>>
    Claim(std::string_view ticket);

  /**
   * @brief Records that the data path has begun executing.
   */
  [[nodiscard]] Result<std::shared_ptr<Session>>
    MarkActive(std::string_view session_id);

  [[nodiscard]] Result<std::shared_ptr<Session>>
    MarkCompleted(std::string_view session_id);

  [[nodiscard]] Result<std::shared_ptr<Session>>
    MarkFailed(std::string_view session_id);

  /**
   * @brief Replaces a session's strongly-typed RDMA parameters.
   */
  void UpdateRdmaParameters(std::string_view session_id,
                            const RdmaParameters& parameters);

  /**
   * @brief Drops sessions whose expire_deadline has elapsed.
   *
   * Returns the number of evictions; intended for periodic invocation by a
   * background sweeper in M2/M3.
   */
  std::size_t SweepExpired(std::chrono::steady_clock::time_point now);

 private:
  Result<std::shared_ptr<Session>>
    Transition(std::string_view session_id, SessionState expected,
               SessionState desired, ErrorCode failure_code);

  std::shared_ptr<spdlog::logger> logger_;
  std::string                     gateway_id_;
  std::string                     gateway_endpoint_;
  std::size_t                     default_chunk_size_{0};
  std::chrono::seconds            session_ttl_{0};

  mutable std::mutex                                                   mu_;
  std::unordered_map<std::string, std::shared_ptr<Session>>           by_id_;
  std::unordered_map<std::string, std::string>                        by_ticket_;
  std::unordered_map<std::string, std::shared_ptr<Session>>           by_idempotency_;
};

}  // namespace us3_turbo_access::gateway::core
