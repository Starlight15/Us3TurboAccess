#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/logger.h>

namespace us3_turbo_access::gateway {

/**
 * @brief Backend stub selectors recognised by the gateway.
 */
enum class BackendKind {
  kMemory,    /**< In-memory KV with a capacity ceiling. */
  kNull,      /**< Discard writes and synthesise empty reads. */
  kComposite, /**< Split index + data via IIndexStore + IDataStore. */
};

/**
 * @brief GDS (cuObjServer) transport tunables.
 *
 * Fields here apply only to the GDS data plane and are independent from
 * the RDMA path.
 */
struct GdsOptions {
  /** RDMA port that cuObjServer listens on for GDS data transfers. */
  int                       rdma_port{18516};
  /**
   * Size buckets (in bytes) for the pre-registered pinned host buffers
   * used to land GDS chunks before forwarding them to the backend.
   */
  std::vector<std::size_t>  buffer_size_classes{
      1ULL * 1024ULL * 1024ULL,
      16ULL * 1024ULL * 1024ULL,
      256ULL * 1024ULL * 1024ULL,
      1ULL * 1024ULL * 1024ULL * 1024ULL,
  };
  /** Number of buffers retained per size class. */
  std::size_t               buffer_max_per_class{4};
  /**
   * Hard limit for a single cuObjServer chunk (1 GiB). Requests above
   * this size are rejected before any DMA is attempted.
   */
  std::size_t               max_chunk_bytes{1ULL * 1024ULL * 1024ULL * 1024ULL};
};

/**
 * @brief Native RDMA transport tunables.
 *
 * Hardware-specific knobs that are wholly independent of the GDS path.
 */
struct RdmaOptions {
  /** Listening port for the native RDMA data plane. */
  int                       listen_port{18515};
  /** RDMA HCA name; empty selects the first active device. */
  std::string               device_name;
  /** InfiniBand port number on the selected HCA. */
  std::uint8_t              ib_port{1};
  /** GID index used for RoCE; ignored on IB. */
  int                       gid_index{0};

  /**
   * Maximum bytes per single RDMA message. The default of 4 GiB matches
   * the NIC WQE addressable limit; tune downward if the HCA reports a
   * smaller per-WQE capacity.
   */
  std::size_t               max_msg_bytes{4ULL * 1024ULL * 1024ULL * 1024ULL};

  /** QPs created per session. Currently only 1 is supported. */
  int                       qp_per_session{1};

  /** Number of threads dispatching librdmacm CM events. */
  int                       cm_event_threads{1};

  /**
   * RDMA session TTL. After BindSession, a session that is neither
   * committed nor aborted within this window is reclaimed by
   * RdmaSessionSweeper. Set to 0 to disable the sweeper.
   */
  std::chrono::seconds      session_ttl{std::chrono::seconds(60)};
  /** Period between RdmaSessionSweeper scans. */
  std::chrono::seconds      session_sweep_interval{std::chrono::seconds(10)};
};

/**
 * @brief Runtime configuration for one gateway instance.
 *
 * Top-level fields are transport-agnostic (identity / brpc / sessions /
 * backend). Transport-specific tunables live in the nested options
 * (gds / rdma) to keep unrelated paths independent.
 */
struct GatewayOptions {
  /** Identifier reported in logs and to clients. */
  std::string gateway_id{"us3-turbo-access-gateway"};

  /** Address brpc binds to (control plane). */
  std::string bind_host{"0.0.0.0"};
  /** Public host advertised to clients in session metadata. */
  std::string public_host{"127.0.0.1"};
  /** brpc control-plane port. */
  int         brpc_port{8080};
  /** Idle timeout for brpc connections; -1 disables. */
  int         idle_timeout_sec{-1};
  /** brpc worker thread count for the control plane. */
  int         num_threads{4};
  /** Background IO pool size used by chunk RPC handlers. */
  int         io_worker_threads{32};

  /** Whether to enable the GDS data plane. */
  bool        gds_enable{true};
  /** GDS-specific tunables. */
  GdsOptions  gds;
  /** Whether to enable the native RDMA data plane. */
  bool        rdma_enable{false};
  /** RDMA-specific tunables. */
  RdmaOptions rdma;

  /** Default lifetime of an open transfer session. */
  std::chrono::seconds session_ttl{std::chrono::minutes(10)};
  /** Period between session-sweeper scans. */
  std::chrono::seconds session_sweep_interval{std::chrono::seconds(30)};

  /** Backend stub selector; see BackendKind. */
  BackendKind backend_kind{BackendKind::kMemory};
  /** Per-backend total byte capacity (memory backend ceiling). */
  std::size_t backend_capacity{4ULL * 1024ULL * 1024ULL * 1024ULL};

  /** Maximum bytes accepted per multipart part. */
  std::size_t multipart_max_part_size{1ULL * 1024ULL * 1024ULL * 1024ULL};
  /**
   * S3-style minimum part size (last part is exempt). Set to 0 to allow
   * arbitrarily small parts for testing.
   */
  std::size_t multipart_min_part_size{5ULL * 1024ULL * 1024ULL};
  /** Multipart upload TTL: orphans older than this are reclaimed. */
  std::chrono::seconds multipart_ttl{std::chrono::minutes(30)};

  /** Optional logger; if null the gateway falls back to a null sink. */
  std::shared_ptr<spdlog::logger> logger;
};

}  // namespace us3_turbo_access::gateway
