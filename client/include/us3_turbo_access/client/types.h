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
 * @brief Data flow types supported by the client.
 *
 * NONE: No data flow selected (default, will be negotiated).
 * GPUDirect: RDMA via cuObjServer (CUDA device memory, GDS).
 * CPUDirect: UCX RDMA (host-pinned memory, native RDMA).
 */
enum class DataFlow {
  NONE,
  GPUDirect,
  CPUDirect,
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
 * @brief Object identity + request context packed together for upload/multipart chains.
 *
 * Use this to pass the common trio (object, data_flow, request attributes) as a
 * single argument instead of repeating individual fields across layers.
 * ObjectId is kept separate and continues to represent pure object identity.
 */
struct ObjectDescriptor {
  ObjectId    object;
  DataFlow    data_flow{DataFlow::NONE};
  std::string checksum_policy{"none"};

  std::optional<std::uint64_t> offset;
  std::optional<std::uint64_t> length;

  std::optional<std::size_t> expected_total_size;
  std::string idempotency_key;
};

/**
 * @brief Progress snapshot reported during a transfer.
 */
struct TransferProgress {
  std::size_t bytes_completed{0};
  std::size_t bytes_total{0};
  DataFlow data_flow{DataFlow::NONE};
};

/**
 * @brief Callback invoked with incremental transfer progress.
 */
using ProgressCallback = std::function<void(const TransferProgress&)>;

/**
 * @brief Request parameters for PutObject operations.
 */
struct PutObjectRequest {
  ObjectId object;
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
  std::unordered_map<std::string, std::string> extra_headers;
  std::string checksum_policy{"none"};   /**< Checksum policy name sent to the service. */
  std::string idempotency_key;            /**< Caller-supplied idempotency token. */
  ProgressCallback progress_callback;
};

/**
 * @brief Request parameters for GetObject operations.
 */
struct GetObjectRequest {
  ObjectId object;
  std::uint64_t offset{0};                 /**< Starting byte offset within the object. */
  std::optional<std::uint64_t> length;    /**< Requested byte count; empty means to the end. */
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
  std::unordered_map<std::string, std::string> extra_headers;
  std::string checksum_policy{"none"};
  ProgressCallback progress_callback;
};

/**
 * @brief Request parameters for HeadObject operations.
 */
struct HeadObjectRequest {
  ObjectId object;
  std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
  std::unordered_map<std::string, std::string> extra_headers;
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
  /** Data flow actually used for the transfer (may differ from request). */
  DataFlow selected_flow{DataFlow::NONE};
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
 * @brief Result returned by a successful StartUpload call.
 *
 * Shared by the public Client::StartUpload API and the internal
 * MetadataClient / UploadCoordinator layers so all layers use one
 * canonical type instead of parallel structs.
 */
struct StartUploadResult {
  /** Opaque upload identifier issued by the gateway. */
  std::string upload_id;
  /** Maximum bytes accepted per part on this upload (gateway-enforced). */
  std::size_t max_part_size{0};
};

/**
 * @brief Local runtime capabilities detected during client initialization.
 */
struct PlatformCapabilities {
  bool cuda_runtime_available{false};  /**< CUDA runtime library is available. */
  bool gpu_available{false};           /**< A compatible GPU device is present. */
  bool cuobject_available{false};      /**< GDS/cuObject prerequisites are available locally. */
};

/** @brief Returns a stable string identifier for the data flow. */
[[nodiscard]] std::string_view ToString(DataFlow flow);
/** @brief Returns a stable string identifier for the buffer type. */
[[nodiscard]] std::string_view ToString(BufferType type);
/** @brief Returns a stable string identifier for the operation type. */
[[nodiscard]] std::string_view ToString(OperationType operation);

}  // namespace us3_turbo_access::client
