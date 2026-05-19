#pragma once

#include <memory>

#include "us3_turbo_access/gateway/options.h"
#include "us3_turbo_access/gateway/result.h"

namespace us3_turbo_access::gateway {

namespace runtime {
class GatewayRuntime;
}  // namespace runtime

/**
 * @brief Top-level shell that owns the gateway runtime.
 *
 * Construct one instance per configuration, call Start() once, then either
 * RunUntilAskedToQuit() (blocking) or arrange external lifecycle and call
 * Shutdown() to release resources.
 */
class GatewayServer {
 public:
  explicit GatewayServer(GatewayOptions options);
  ~GatewayServer();

  GatewayServer(const GatewayServer&) = delete;
  GatewayServer& operator=(const GatewayServer&) = delete;
  GatewayServer(GatewayServer&&) = delete;
  GatewayServer& operator=(GatewayServer&&) = delete;

  /**
   * @brief Initialises dependencies and starts the brpc server.
   */
  [[nodiscard]] Result<bool> Start();

  /**
   * @brief Releases all resources acquired by Start().
   *
   * Safe to call when Start() failed or was never invoked.
   */
  void Shutdown();

  /**
   * @brief Blocks until the underlying brpc server is asked to quit.
   */
  void RunUntilAskedToQuit();

 private:
  std::unique_ptr<runtime::GatewayRuntime> runtime_;
};

}  // namespace us3_turbo_access::gateway
