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
 * GDS 路径专属配置。当前 GDS 走 cuObject SDK，没什么可调参数。
 * 留作扩展位，避免后续加 GDS 配置时再次改 ClientOptions 顶层结构。
 */
struct GdsClientOptions {
  // 占位：未来可加 cuObj 调度/缓冲/checksum 等。
};

/**
 * HTTP 路径专属配置。所有超时仍走顶层 default_timeout。
 */
struct HttpClientOptions {
  // V2 — 并发下载
  std::size_t parallel_get_threshold{16ULL * 1024 * 1024};  // 单 GET 触发并发分片的最小长度
  std::size_t parallel_get_chunks{8};                        // 每个大对象拆几路并发

  // V2 — 校验和
  bool send_crc32c{true};            // PUT/UploadPart 时算 CRC32C 注入 header
  bool verify_response_crc32c{true}; // 收到 x-amz-checksum-crc32c 时对比 client 算的（占位，本期未使用）

  // V2 — 重试（HttpDataClient 各方法外层 wrapper 使用）
  int max_retry_attempts{3};
  std::chrono::milliseconds retry_initial_backoff{std::chrono::milliseconds(100)};
  std::chrono::milliseconds retry_max_backoff{std::chrono::milliseconds(2000)};
};

/**
 * Native RDMA 路径专属配置。所有参数都是硬件相关或连接池相关。
 * 不与 GDS 共享。
 */
struct RdmaClientOptions {
  // 设备选择
  std::string device_name;                   // 空 = auto，取第一个 active HCA
  int         gid_index{0};                  // RoCE 必填；IB 默认 0
  std::uint8_t ib_port{1};

  // 单次传输上限（按 NIC WQE / max_msg 决定，默认 4 GiB）
  std::size_t max_msg_bytes{4ULL * 1024ULL * 1024ULL * 1024ULL};

  // 连接池（C4b 起生效）
  std::size_t pool_max_idle_per_endpoint{8};
  std::chrono::milliseconds pool_idle_timeout{std::chrono::seconds(60)};
  std::chrono::milliseconds connect_timeout{std::chrono::seconds(5)};

  // 完成线程（C5a 起生效）
  int  cq_size{4096};
  bool dedicated_completion_thread{true};
};

/**
 * @brief 客户端实例配置。
 *
 * 顶层只保留与具体 path 无关的参数（endpoint / timeout / logger 等）；
 * 路径相关参数收纳到对应子结构（gds / rdma），避免不同硬件路径配置互相污染。
 */
struct ClientOptions {
  std::string endpoint;
  std::string client_id{"us3-turbo-access-client"};
  std::string bearer_token;
  std::unordered_map<std::string, std::string> default_headers;
  std::chrono::milliseconds default_timeout{std::chrono::milliseconds(30000)};
  DataPath data_path{DataPath::kGdsCuObject};
  std::shared_ptr<spdlog::logger> logger;

  // *Async API 用的线程池大小；0 = 由 ClientCore 默认（hardware_concurrency 折半，至少 1）。
  std::size_t async_worker_threads{0};

  // 路径专属子配置
  GdsClientOptions  gds;
  RdmaClientOptions rdma;
  HttpClientOptions http;
};

}  // namespace us3_turbo_access::client
