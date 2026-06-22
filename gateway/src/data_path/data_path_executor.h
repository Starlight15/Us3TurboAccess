#pragma once

#include <string>

#include "us3_turbo_access/gateway/result.h"
#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway::core {
class Session;
}  // namespace us3_turbo_access::gateway::core

namespace us3_turbo_access::gateway::data_flow {

/**
 * @brief Common contract for every server-side data-path implementation
 *        (HTTP / GDS / native RDMA / future variants).
 *
 * Concrete executors expose path-specific transfer methods on top of this
 * interface. The interface covers what the runtime needs to manage lifecycle,
 * what the control plane needs to expose to clients, and the per-session
 * negotiation hook called by SessionOpener when a session is created.
 */
class IDataPathExecutor {
 public:
  virtual ~IDataPathExecutor() = default;

  IDataPathExecutor(const IDataPathExecutor&) = delete;
  IDataPathExecutor& operator=(const IDataPathExecutor&) = delete;

  /** @brief Wire identifier for the flow this executor implements. */
  [[nodiscard]] virtual DataFlow kind() const noexcept = 0;

  /** @brief True once Start() has produced a usable transport. */
  [[nodiscard]] virtual bool available() const = 0;

  /** @brief Public endpoint clients should connect to once available. */
  [[nodiscard]] virtual std::string endpoint() const = 0;

  /** @brief Bring the underlying transport up. Idempotent. */
  [[nodiscard]] virtual Result<bool> Start() = 0;

  /** @brief Release the transport. Idempotent. */
  virtual void Stop() = 0;

  /**
   * @brief Called by SessionOpener right after the session is registered.
   *
   * Implementations can perform path-specific setup (backend reservation,
   * availability check, ...). Returning a failure aborts the open.
   *
   * Default implementation: no-op, returns success.
   */
  [[nodiscard]] virtual Result<bool>
    OnSessionOpened(const core::Session& session);

 protected:
  IDataPathExecutor() = default;
};

}  // namespace us3_turbo_access::gateway::data_flow
