#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <spdlog/logger.h>

#include "us3_turbo_access/gateway/result.h"

namespace us3_turbo_access::gateway::core {

class Session;
class SessionStore;

/**
 * @brief Application-service layer for session lifecycle operations.
 *
 * Provides the API layer with a focused interface for session resolution
 * and state transitions, so that ControlPlaneService does not need to
 * depend on SessionStore directly.
 *
 * Why this layer exists:
 *   Previously, ControlPlaneService directly called SessionStore methods
 *   (Find, FindByTicket, BumpActive, MarkFailed) and contained its own
 *   ResolveSession helper. This leaked session store details into the
 *   API layer. SessionAppService absorbs those responsibilities so that
 *   ControlPlaneService remains a thin protocol adapter.
 *
 * What this layer does NOT do:
 *   - Session creation (handled by SessionOpener)
 *   - Session sweeping (handled by SessionSweeper)
 *   - Data-path session setup (handled by UcxExecutor / GdsExecutor)
 */
class SessionAppService {
 public:
  SessionAppService(SessionStore& sessions,
                    std::shared_ptr<spdlog::logger> logger);

  /**
   * @brief Resolve a GDS chunk request to a session.
   *
   * Tries session_id first, then transfer_ticket.
   * Returns nullptr if neither resolves.
   */
  [[nodiscard]] std::shared_ptr<Session>
    ResolveForGdsChunk(std::string_view session_id,
                       std::string_view transfer_ticket) const;

  /**
   * @brief Idempotent activation: CAS kOpened→kActive; no-op if already active.
   */
  void BumpActive(std::string_view session_id) const;

  /**
   * @brief Mark a session as failed by shared_ptr.
   *
   * Convenience wrapper that extracts the session_id.
   */
  void MarkFailed(const Session& session) const;

  /**
   * @brief Mark a session as failed by session_id.
   *
   * @return true if the session was found and marked, false if not found.
   */
  [[nodiscard]] bool MarkFailedById(std::string_view session_id) const;

 private:
  SessionStore&                    sessions_;
  std::shared_ptr<spdlog::logger>  logger_;
};

}  // namespace us3_turbo_access::gateway::core
