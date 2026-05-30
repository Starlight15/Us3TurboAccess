#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "us3_turbo_access/common/error_code.h"

namespace us3_turbo_access::client {

/**
 * @brief Error codes returned by the public client API.
 *
 * The enum is an alias of the project-wide canonical enum so the same code
 * values flow between the client SDK and the gateway without translation.
 */
using ErrorCode = ::us3_turbo_access::common::ErrorCode;

/**
 * @brief Data transfer paths supported by the client.
 *
 * kHttpTcp talks standard HTTP/1.1 to the gateway's http_master_service and
 * only accepts BufferType::kHostRegular. kGdsCuObject and kNativeRdma are
 * independent transports with their own buffer-type requirements (cuda
 * device memory for GDS, host-pinned for RDMA).
 */
enum class DataPath {
  kGdsCuObject,
  kNativeRdma,
  kHttpTcp,
};

/**
 * @brief Memory buffer categories accepted by transfer APIs.
 */
enum class BufferType {
  kHostRegular,
  kHostPinned,
  kCudaDevice,
};

/**
 * @brief Object operations issued through the client.
 */
enum class OperationType {
  kGet,
  kPut,
  kHead,
};

/**
 * @brief Object identifier within a bucket namespace.
 */
struct ObjectId {
  std::string bucket;
  std::string key;
};

/**
 * @brief Progress snapshot reported during a transfer.
 */
struct TransferProgress {
  std::size_t bytes_completed{0};
  std::size_t bytes_total{0};
  DataPath data_path{DataPath::kGdsCuObject};
};

/**
 * @brief Callback invoked with incremental transfer progress.
 */
using ProgressCallback = std::function<void(const TransferProgress&)>;

/**
 * @brief Per-request options for object transfer operations.
 */
struct RequestOptions {
  ObjectId object;
  std::uint64_t offset{0};                 /**< Starting byte offset within the object. */
  std::optional<std::uint64_t> length;    /**< Requested byte count; empty means to the end. */
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
  std::unordered_map<std::string, std::string> extra_headers;
  std::string checksum_policy{"none"};   /**< Checksum policy name sent to the service. */
  std::string idempotency_key;            /**< Caller-supplied idempotency token. */
  ProgressCallback progress_callback;
};

/**
 * @brief Mutable data buffer supplied to download operations.
 */
struct MutableBufferView {
  void* data{nullptr};
  std::size_t size{0};
  BufferType type{BufferType::kHostRegular};
};

/**
 * @brief Read-only data buffer supplied to upload operations.
 */
struct ConstBufferView {
  const void* data{nullptr};
  std::size_t size{0};
  BufferType type{BufferType::kHostRegular};
};

/**
 * @brief Object metadata returned by head requests.
 */
struct ObjectMetadata {
  std::size_t content_length{0};
  std::string etag;
  std::string version;
  std::unordered_map<std::string, std::string> headers;
};

/**
 * @brief Transfer result returned by successful upload and download operations.
 */
struct TransferOutcome {
  /** Data path actually used for the transfer (may differ from request). */
  DataPath selected_path{DataPath::kGdsCuObject};
  /** Total bytes transferred. */
  std::size_t bytes_transferred{0};
  /** Service-side request identifier, useful for log correlation. */
  std::string request_id;
  /** Transfer session identifier (control plane). */
  std::string session_id;
  /** Server-reported terminal status for this transfer (e.g. "completed"). */
  std::string transfer_status;
  /** Identifier of the gateway that handled the transfer. */
  std::string gateway_id;
  /** Path-specific reply string (RDMA path echoes acknowledgement data here). */
  std::string rdma_reply;
  /** ETag assigned by the backend on PUT/UploadPart, empty on GET. */
  std::string etag;
  /** Object version assigned by the backend, when versioning is enabled. */
  std::string version;
  /**
   * Server-computed CRC32C：
   *   HTTP PUT / UploadPart  : 来自响应头 x-amz-checksum-crc32c
   *   HTTP GET               : 来自响应头（P1.3 后），client 端边读边算并比对
   *   RDMA PUT/PUT_PART/GET  : 来自 CommitObject/CommitPart/CommitGet 响应字段
   *   GDS PUT/GET           : 来自 GdsChunkResponse.crc32c
   *
   * GET 路径下若两端 CRC 不一致，TransferPath 直接返回 kInvalidArgument
   * 错误（不会把结果交给调用方）；PUT 路径下 server 校验失败也是同样语义。
   */
  std::optional<std::uint32_t> server_crc32c;
};

/**
 * @brief Local runtime capabilities detected during client initialization.
 */
struct PlatformCapabilities {
  bool cuda_runtime_available{false};  /**< CUDA runtime library is available. */
  bool gpu_available{false};           /**< A compatible GPU device is present. */
  bool cuobject_available{false};      /**< GDS/cuObject prerequisites are available locally. */
};

/** @brief Returns a stable string identifier for the data path. */
[[nodiscard]] std::string_view ToString(DataPath path);
/** @brief Returns a stable string identifier for the buffer type. */
[[nodiscard]] std::string_view ToString(BufferType type);
/** @brief Returns a stable string identifier for the operation type. */
[[nodiscard]] std::string_view ToString(OperationType operation);

}  // namespace us3_turbo_access::client
