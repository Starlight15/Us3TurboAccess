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
 */
enum class DataPath {
  kGdsCuObject,
  kNativeRdma,
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
  DataPath selected_path{DataPath::kGdsCuObject}; /**< Data path used for the transfer. */
  std::size_t bytes_transferred{0};               /**< Total bytes transferred. */
  std::string request_id;                         /**< Service request identifier. */
  std::string session_id;                         /**< Transfer session identifier. */
  std::string transfer_status;
  std::string gateway_id;
  std::string rdma_reply;
  std::string etag;
  std::string version;
};

/**
 * @brief Local runtime capabilities detected during client initialization.
 */
struct PlatformCapabilities {
  bool cuda_runtime_available{false};  /**< CUDA runtime library is available. */
  bool gpu_available{false};           /**< A compatible GPU device is present. */
  bool cuobject_available{false};      /**< GDS/cuObject prerequisites are available locally. */
};

[[nodiscard]] std::string_view ToString(DataPath path);
[[nodiscard]] std::string_view ToString(BufferType type);
[[nodiscard]] std::string_view ToString(OperationType operation);

}  // namespace us3_turbo_access::client
