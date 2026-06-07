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

/** UCX 数据通路参数（与 verbs RDMA 完全独立）。 */
struct UcxOptions {
  /** UCX listener port（供 client ucp_ep_create 连接）。 */
  int         listen_port{18520};
  /**
   * 最大单笔传输字节数；用于 PrepareTransfer 告知 client 上限，
   * 超出时 PrepareTransfer 提前拒绝，避免分配超大 buffer。
   */
  std::size_t max_msg_bytes{4ULL * 1024ULL * 1024ULL * 1024ULL};
  /** CommitObject 等待 ep_ready + GET 完成的总 deadline；超时返回 kTimeout（retryable）。 */
  std::chrono::milliseconds commit_data_timeout{std::chrono::milliseconds(5000)};
  /** buffer pool 最大空闲数；0 = 不复用（每次 malloc+mlock）。 */
  std::size_t buffer_pool_max_idle{16};
  /** Session 超时时间（PrepareTransfer 分配 buffer 到 CommitObject）。 */
  std::chrono::seconds session_ttl{std::chrono::seconds(120)};
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
  /**
   * Maximum concurrent requests allowed by brpc server.
   * 0 = unlimited (default). Set to a positive value to enable rate limiting.
   * When exceeded, brpc returns 503 Service Unavailable.
   */
  int         max_concurrency{0};
  /** Background IO pool size used by chunk RPC handlers. */
  int         io_worker_threads{32};

  /** Whether to enable the GDS data plane. */
  bool        gds_enable{true};
  /** GDS-specific tunables. */
  GdsOptions  gds;
  /** Whether to enable the UCX data plane. */
  bool        ucx_enable{false};
  /** UCX-specific tunables. */
  UcxOptions  ucx;

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

  /**
   * 单次 HTTP PUT body 上限（覆盖单对象 PUT 与 multipart UploadPart 两条入口）。
   * 超过此值 HttpFrontend 直接返回 413 + kPayloadTooLarge。
   * brpc 框架层 max_body_size 会被抬到 http_max_put_bytes + 少量 header 余量，
   * 保证所有"超限"路径都由 HttpFrontend 自己统一处理，而不是被 brpc 提前拒。
   * 默认 1 GiB —— 与 cuObjServer 单次传输上限对齐。
   */
  std::size_t http_max_put_bytes{1ULL * 1024ULL * 1024ULL * 1024ULL};

  /** Optional logger; if null the gateway falls back to a null sink. */
  std::shared_ptr<spdlog::logger> logger;
};

}  // namespace us3_turbo_access::gateway
