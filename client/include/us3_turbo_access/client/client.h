#pragma once

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"
#include "us3_turbo_access/client/status.h"
#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

class ClientCore;
class Client;

/**
 * @brief Final manifest returned after a successful multipart Complete.
 */
struct CompleteUploadResult {
  /** Canonical ETag of the assembled object. */
  std::string etag;
  /** Object version assigned by the backend, when versioning is enabled. */
  std::string version;
  /** Total assembled object size in bytes. */
  std::size_t content_length{0};
};

/**
 * @brief Handle representing an in-progress multipart upload.
 *
 * Returned by Client::StartUpload. The handle owns the server-side upload
 * state until Complete() or Abort() is invoked, or until the handle is
 * destroyed (in which case the destructor issues a best-effort Abort).
 *
 * Thread-compat: a handle is owned by a single logical uploader. Parts can
 * be submitted serially via UploadPart, or in parallel through UploadParts.
 *
 * Lifetime: callers must call either Complete() or Abort() exactly once.
 * Dropping the handle without doing so triggers a best-effort Abort from
 * the destructor but does not surface failures.
 */
class MultipartUpload {
 public:
  // 默认构造产生一个空 handle；在 Client::StartUpload 成功后被 move 填充。
  MultipartUpload();
  ~MultipartUpload();
  MultipartUpload(const MultipartUpload&) = delete;
  MultipartUpload& operator=(const MultipartUpload&) = delete;
  MultipartUpload(MultipartUpload&&) noexcept;
  MultipartUpload& operator=(MultipartUpload&&) noexcept;

  /** @brief Returns the gateway-issued upload identifier. */
  [[nodiscard]] const std::string& upload_id() const noexcept;

  /** @brief Returns the gateway-enforced upper bound on per-part size. */
  [[nodiscard]] std::size_t max_part_size() const noexcept;

  /**
   * @brief Sets the per-chunk checksum policy applied to subsequent parts.
   *
   * @param policy "none" disables checksums; "md5" computes an MD5 digest
   *               per chunk and asks the gateway to verify it.
   */
  void set_checksum_policy(std::string policy);

  /**
   * @brief Uploads a single part of the multipart object.
   *
   * @param part_number   1-based part index; must be unique within the upload.
   * @param object_offset Byte offset of this part within the final object.
   * @param buffer        Source buffer; must remain valid until this call
   *                      returns.
   * @return Transfer outcome including the part ETag required at Complete().
   */
  [[nodiscard]] Result<TransferOutcome>
    UploadPart(std::uint32_t part_number, std::uint64_t object_offset,
               ConstBufferView buffer);

  /**
   * @brief Descriptor for a single part to be uploaded by UploadParts.
   *
   * Each PartSpec must reference a distinct device buffer because the
   * GDS/RDMA backends register memory per outstanding part.
   */
  struct PartSpec {
    /** 1-based part index. */
    std::uint32_t   part_number{0};
    /** Byte offset of this part within the assembled object. */
    std::uint64_t   object_offset{0};
    /** Source buffer for this part. */
    ConstBufferView buffer{};
  };

  /**
   * @brief Uploads multiple parts in parallel.
   *
   * Launches up to @p concurrency parallel uploads. On the first failure the
   * remaining in-flight uploads still drain so resources are released; the
   * first error encountered is returned.
   *
   * @param parts       Parts to upload; each must reference a distinct buffer.
   * @param concurrency Number of parts uploaded in parallel.
   * @return Per-part TransferOutcome on success, or the first error on
   *         failure.
   */
  [[nodiscard]] Result<std::vector<TransferOutcome>>
    UploadParts(const std::vector<PartSpec>& parts, std::size_t concurrency);

  /**
   * @brief Finalises the upload on the gateway and assembles the object.
   *
   * Must be called after all parts have been uploaded. Idempotent on the
   * gateway side when the same upload_id is presented.
   */
  [[nodiscard]] Result<CompleteUploadResult> Complete();

  /**
   * @brief Aborts the upload and releases any gateway-side state.
   *
   * @return Result::Success(true) when the abort was acknowledged, even if
   *         the upload had already been completed or aborted previously.
   */
  [[nodiscard]] Result<bool> Abort();

 private:
  friend class Client;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit MultipartUpload(std::unique_ptr<Impl> impl);
};

/**
 * @brief Top-level client SDK entry point.
 *
 * One Client per process is typical. The instance owns the configured
 * transport (HTTP / RDMA / GDS) and the executor backing the *Async
 * methods. Public methods are thread-safe unless documented otherwise.
 */
class Client {
 public:
  /**
   * @brief Constructs a client with the provided configuration.
   *
   * The constructor does not perform network I/O. Call Initialize() to
   * complete startup before invoking any transfer method.
   */
  explicit Client(ClientOptions options);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

  /**
   * @brief Initialises the client (resolves endpoints, opens pools).
   *
   * Idempotent; subsequent calls return Success(true) without side effects.
   * Must complete successfully before any other method is invoked.
   */
  [[nodiscard]] Result<bool> Initialize();

  /**
   * @brief Releases all client resources.
   *
   * Safe to call multiple times. After Shutdown() the client cannot be
   * re-initialised; create a new Client instance instead.
   */
  void Shutdown();

  /** @brief Returns true after a successful Initialize() call. */
  [[nodiscard]] bool initialized() const;

  /** @brief Returns runtime capabilities detected during Initialize(). */
  [[nodiscard]] const PlatformCapabilities& capabilities() const;

  /**
   * @brief Issues a HEAD request for the object.
   *
   * @return Object metadata including content length, etag, and version.
   */
  [[nodiscard]] Result<ObjectMetadata> HeadObject(const std::string& bucket,
                                                  const std::string& key) const;

  /**
   * @brief Downloads the object (or a range of it) into the caller's buffer.
   *
   * @param request GetObjectRequest specifying object id, offset, length and
   *                progress callback.
   * @param buffer  Destination buffer; must be at least request.length bytes.
   */
  [[nodiscard]] Result<TransferOutcome> GetObject(const GetObjectRequest& request,
                                                  MutableBufferView buffer) const;

  /**
   * @brief Uploads the contents of @p buffer as the named object.
   *
   * @param request PutObjectRequest identifying the destination object.
   * @param buffer  Source buffer; must remain valid until this call returns.
   */
  [[nodiscard]] Result<TransferOutcome> PutObject(const PutObjectRequest& request,
                                                  ConstBufferView buffer) const;

  /**
   * @brief Asynchronous variant of HeadObject.
   *
   * Submits the work to the shared executor and returns immediately. The
   * caller must keep the returned future alive until it is ready.
   */
  [[nodiscard]] std::future<Result<ObjectMetadata>>
    HeadObjectAsync(const std::string& bucket, const std::string& key) const;

  /**
   * @brief Asynchronous variant of GetObject.
   *
   * @note The caller must keep @p buffer valid until the returned future
   *       completes; only the view is copied, the underlying memory is not.
   */
  [[nodiscard]] std::future<Result<TransferOutcome>>
    GetObjectAsync(const GetObjectRequest& request, MutableBufferView buffer) const;

  /**
   * @brief Asynchronous variant of PutObject.
   *
   * @note The caller must keep @p buffer valid until the returned future
   *       completes; only the view is copied, the underlying memory is not.
   */
  [[nodiscard]] std::future<Result<TransferOutcome>>
    PutObjectAsync(const PutObjectRequest& request, ConstBufferView buffer) const;

  /**
   * @brief Initiates a multipart upload.
   *
   * On success, writes the upload handle into @p out.
   * On failure, @p out is untouched and the returned Status describes the error.
   *
   * @param bucket              Destination bucket.
   * @param key                 Destination object key.
   * @param out                 Non-null pointer to a MultipartUpload to fill.
   * @param expected_total_size Optional hint for the gateway; pass 0 if unknown.
   * @param idempotency_key     Caller-supplied token.
   */
  [[nodiscard]] Status
    StartUpload(const std::string& bucket, const std::string& key,
                MultipartUpload* out,
                std::size_t expected_total_size = 0,
                const std::string& idempotency_key = {});

  /**
   * @brief Pre-registers a GPU device buffer with the cuObject descriptor
   *        table (GDS path optimisation).
   *
   * On the GDS path each PUT/GET requires the buffer to be pinned into the
   * GPU BAR1 region via cuMemObjGetDescriptor. Without an explicit
   * registration the SDK pins lazily on the first transfer; this lazy pin
   * involves a millisecond-scale syscall (nvidia_p2p_get_pages) on the
   * hot path. Calling this method right after cudaMalloc amortises the
   * pin cost outside the transfer path.
   *
   * @param ptr  Device pointer obtained from cudaMalloc.
   * @param size Region size in bytes.
   *
   * @note Idempotent: re-registering the same ptr returns Success.
   * @note On non-GDS transports this call is a no-op that returns Success.
   * @see Client::UnregisterDeviceBuffer
   */
  [[nodiscard]] Result<bool> RegisterDeviceBuffer(void* ptr, std::size_t size);

  /**
   * @brief Releases a buffer previously registered by RegisterDeviceBuffer.
   *
   * @warning Must be called BEFORE cudaFree(ptr). NVIDIA documentation
   *          warns that freeing CUDA memory while the descriptor is still
   *          held by cuObjClient can deadlock the nvidia-fs kernel module.
   *
   * @note Idempotent: unregistering an unknown ptr returns Success.
   * @note On non-GDS transports this call is a no-op that returns Success.
   */
  [[nodiscard]] Result<bool> UnregisterDeviceBuffer(void* ptr);

 private:
  std::unique_ptr<ClientCore> core_;
};

}  // namespace us3_turbo_access::client
