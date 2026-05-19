#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include <spdlog/logger.h>

namespace us3_turbo_access::gateway {

/**
 * @brief Backend stub selectors recognised by the gateway.
 */
enum class BackendKind {
  kMemory,    /**< In-memory KV with capacity ceiling.   */
  kNull,      /**< Discard writes / synthesize empty reads. */
};

/**
 * @brief Runtime configuration for a single gateway instance.
 *
 * brpc serves HTTP and Protobuf services on the same port. RDMA is optional;
 * when enabled, startup fails fast if the device cannot be opened.
 */
struct GatewayOptions {
  // Identity
  std::string gateway_id{"us3-turbo-access-gateway"};

  // Network
  std::string bind_host{"0.0.0.0"};
  std::string public_host{"127.0.0.1"};
  int         brpc_port{8080};
  int         idle_timeout_sec{-1};
  int         num_threads{4};

  // RDMA / GDS
  bool        rdma_enable{false};      /**< Native-RDMA path (M2). */
  bool        gds_enable{true};        /**< GDS / cuObjServer data path (M1). */
  std::string rdma_device;
  int         rdma_port{18515};
  int         gds_rdma_port{0};        /**< 0 means rdma_port + 1. */

  // Transfer planning
  std::size_t default_chunk_size{8U * 1024U * 1024U};
  std::chrono::seconds session_ttl{std::chrono::minutes(10)};

  // Backend
  BackendKind backend_kind{BackendKind::kMemory};
  std::size_t backend_capacity{4ULL * 1024ULL * 1024ULL * 1024ULL};

  // Logging
  std::shared_ptr<spdlog::logger> logger;
};

}  // namespace us3_turbo_access::gateway
