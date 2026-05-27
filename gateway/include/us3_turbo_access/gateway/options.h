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
  kMemory,    /**< In-memory KV with capacity ceiling.   */
  kNull,      /**< Discard writes / synthesize empty reads. */
  kComposite, /**< Split index + data via IIndexStore + IDataStore (current: Null stubs). */
};

/**
 * GDS（cuObjServer）路径专属配置。所有参数仅影响 GDS 数据面，与 RDMA 路径无关。
 */
struct GdsOptions {
  int                       rdma_port{18516};
  /** pinned + RDMA-registered host buffer 的 size class 列表 */
  std::vector<std::size_t>  buffer_size_classes{
      1ULL * 1024ULL * 1024ULL,
      16ULL * 1024ULL * 1024ULL,
      256ULL * 1024ULL * 1024ULL,
      1ULL * 1024ULL * 1024ULL * 1024ULL,
  };
  std::size_t               buffer_max_per_class{4};
  /** cuObjServer 单次硬限 1 GiB；不可超 */
  std::size_t               max_chunk_bytes{1ULL * 1024ULL * 1024ULL * 1024ULL};
};

/**
 * Native RDMA 路径专属配置。硬件相关，与 GDS 路径完全独立。
 */
struct RdmaOptions {
  int                       listen_port{18515};
  std::string               device_name;       // 空 = auto
  std::uint8_t              ib_port{1};
  int                       gid_index{0};

  /** 单个 session 单次 RDMA 消息上限（默认 4 GiB；按 NIC WQE 实际能力调） */
  std::size_t               max_msg_bytes{4ULL * 1024ULL * 1024ULL * 1024ULL};

  /** 每 session 的 QP 数；初版只用 1 */
  int                       qp_per_session{1};

  /** CM event 处理线程数（C5 之后会用上） */
  int                       cm_event_threads{1};

  /** RDMA session 超时：BindSession 后未 commit 也未 abort 视为孤儿，
   *  由 RdmaSessionSweeper 回收。0 表示禁用 sweeper。*/
  std::chrono::seconds      session_ttl{std::chrono::seconds(60)};
  std::chrono::seconds      session_sweep_interval{std::chrono::seconds(10)};
};

/**
 * @brief Runtime configuration for a single gateway instance.
 *
 * 顶层只放与具体数据通路无关的项（identity / brpc / session / backend）；
 * 每条数据通路的参数收纳到对应子结构（gds / rdma），互不污染。
 */
struct GatewayOptions {
  // Identity
  std::string gateway_id{"us3-turbo-access-gateway"};

  // Network (control plane)
  std::string bind_host{"0.0.0.0"};
  std::string public_host{"127.0.0.1"};
  int         brpc_port{8080};
  int         idle_timeout_sec{-1};
  int         num_threads{4};
  int         io_worker_threads{32};   /**< 后台 IO 池大小，跑 chunk RPC */

  // 数据面开关 + 各路径子配置
  bool        gds_enable{true};
  GdsOptions  gds;
  bool        rdma_enable{false};
  RdmaOptions rdma;

  // Session
  std::chrono::seconds session_ttl{std::chrono::minutes(10)};
  std::chrono::seconds session_sweep_interval{std::chrono::seconds(30)};

  // Backend
  BackendKind backend_kind{BackendKind::kMemory};
  std::size_t backend_capacity{4ULL * 1024ULL * 1024ULL * 1024ULL};

  // Multipart upload
  std::size_t multipart_max_part_size{1ULL * 1024ULL * 1024ULL * 1024ULL};
  /** S3-style 最小 part size（除末尾外）；0 关掉，便于测试小 part */
  std::size_t multipart_min_part_size{5ULL * 1024ULL * 1024ULL};
  std::chrono::seconds multipart_ttl{std::chrono::minutes(30)};

  // Logging
  std::shared_ptr<spdlog::logger> logger;
};

}  // namespace us3_turbo_access::gateway
