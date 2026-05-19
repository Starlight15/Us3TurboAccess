#pragma once

#include <string>

#include "us3_turbo_access/gateway/result.h"
#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway::data_path {

/**
 * @brief Common contract for every server-side data-path implementation
 *        (GDS / native RDMA / future variants).
 *
 * Concrete executors expose path-specific transfer methods on top of this
 * interface. The interface itself only covers what the runtime needs to
 * manage lifecycle and what the control plane needs to expose to clients.
 */
class IDataPathExecutor {
 public:
  virtual ~IDataPathExecutor() = default;

  IDataPathExecutor(const IDataPathExecutor&) = delete;
  IDataPathExecutor& operator=(const IDataPathExecutor&) = delete;

  /** @brief Wire identifier for the path this executor implements. */
  [[nodiscard]] virtual DataPath kind() const noexcept = 0;

  /** @brief True once Start() has produced a usable transport. */
  [[nodiscard]] virtual bool available() const = 0;

  /** @brief Public endpoint clients should connect to once available. */
  [[nodiscard]] virtual std::string endpoint() const = 0;

  /** @brief Bring the underlying transport up. Idempotent. */
  [[nodiscard]] virtual Result<bool> Start() = 0;

  /** @brief Release the transport. Idempotent. */
  virtual void Stop() = 0;

 protected:
  IDataPathExecutor() = default;
};

}  // namespace us3_turbo_access::gateway::data_path
