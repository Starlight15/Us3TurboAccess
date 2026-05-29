#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <spdlog/logger.h>

#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

/**
 * @brief GDS-specific client tuning.
 *
 * The current GDS transport delegates scheduling to the cuObject SDK and
 * exposes no tunables. This struct is reserved so future GDS options can
 * be added without breaking the top-level ClientOptions layout.
 */
struct GdsClientOptions {
  // Reserved for future cuObject scheduling / buffer / checksum tuning.
};

/**
 * @brief HTTP transport tunables.
 *
 * Common timeouts continue to flow through ClientOptions::default_timeout;
 * fields here are HTTP-only behaviours.
 */
struct HttpClientOptions {
  /** Object length above which GetObject splits into parallel sub-ranges. */
  std::size_t parallel_get_threshold{16ULL * 1024 * 1024};
  /** Number of parallel sub-ranges issued by split GETs. */
  std::size_t parallel_get_chunks{8};

  /** Inject x-amz-checksum-crc32c on PUT / UploadPart. */
  bool send_crc32c{true};
  /** Verify server-echoed x-amz-checksum-crc32c against the client value. */
  bool verify_response_crc32c{true};

  /** Maximum total attempts (including the initial one) for retried calls. */
  int max_retry_attempts{3};
  /** Initial exponential backoff delay between retries. */
  std::chrono::milliseconds retry_initial_backoff{std::chrono::milliseconds(100)};
  /** Upper bound for the exponential backoff between retries. */
  std::chrono::milliseconds retry_max_backoff{std::chrono::milliseconds(2000)};

  /**
   * 单次 PutObject 接受的最大 body 字节数。超过此值在 client 端立刻返回
   * kPayloadTooLarge，避免把超大 buffer 推到 TCP 再被 server 413。
   * 默认 5 GiB —— 与 AWS S3 单段 PUT 服务端硬限对齐；如果 gateway 端
   * http_max_put_bytes 更严（默认 1 GiB），实际仍以 server 端为准。
   * 大于此值的对象请改用 StartUpload + UploadPart 分片上传。
   */
  std::size_t put_single_max_bytes{5ULL * 1024ULL * 1024ULL * 1024ULL};
};

/**
 * @brief Native RDMA transport tunables (hardware + connection pool).
 *
 * Independent from GdsClientOptions: the two transports use different
 * libraries (libibverbs vs cuObject) and do not share state.
 */
struct RdmaClientOptions {
  /** RDMA HCA name; empty selects the first active device on the host. */
  std::string device_name;
  /** GID index used for RoCE; ignored on IB. */
  int         gid_index{0};
  /** InfiniBand port number to open on the selected HCA. */
  std::uint8_t ib_port{1};

  /** Maximum bytes per single RDMA transfer; capped by NIC WQE / max_msg. */
  std::size_t max_msg_bytes{4ULL * 1024ULL * 1024ULL * 1024ULL};

  /** Maximum idle connections kept per (gateway endpoint) bucket. */
  std::size_t pool_max_idle_per_endpoint{8};
  /** Idle timeout before a pooled connection is reclaimed. */
  std::chrono::milliseconds pool_idle_timeout{std::chrono::seconds(60)};
  /** Timeout for the initial RDMA-CM resolve+connect handshake. */
  std::chrono::milliseconds connect_timeout{std::chrono::seconds(5)};

  /** Size of the libibverbs completion queue. */
  int  cq_size{4096};
  /** Run the CQ poller in a dedicated thread instead of inline. */
  bool dedicated_completion_thread{true};
};

/**
 * @brief Client instance configuration.
 *
 * Top-level fields are transport-agnostic (endpoint / timeout / logger).
 * Transport-specific knobs live in the nested options (gds / rdma / http)
 * so unrelated paths do not pollute each other's namespace.
 */
struct ClientOptions {
  /** Gateway endpoint in "host:port" form. */
  std::string endpoint;
  /** Identifier reported to the gateway for telemetry and logs. */
  std::string client_id{"us3-turbo-access-client"};
  /** Optional bearer token included in outbound requests. */
  std::string bearer_token;
  /** Headers attached to every outbound HTTP request. */
  std::unordered_map<std::string, std::string> default_headers;
  /** Default per-request timeout when RequestOptions::timeout is unset. */
  std::chrono::milliseconds default_timeout{std::chrono::milliseconds(30000)};
  /** Data transport selected for transfer operations. */
  DataPath data_path{DataPath::kGdsCuObject};
  /** Optional logger; if null, the client falls back to a null sink. */
  std::shared_ptr<spdlog::logger> logger;

  /**
   * Worker threads backing the *Async API. Set to 0 to let ClientCore pick
   * a default (hardware_concurrency / 2, minimum 1).
   */
  std::size_t async_worker_threads{0};

  /** GDS-specific tunables. */
  GdsClientOptions  gds;
  /** RDMA-specific tunables. */
  RdmaClientOptions rdma;
  /** HTTP-specific tunables. */
  HttpClientOptions http;
};

}  // namespace us3_turbo_access::client
