#pragma once

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

class ClientCore;
class Client;

struct StartUploadResult {
  std::string upload_id;
  std::size_t max_part_size{0};
};

struct CompleteUploadResult {
  std::string etag;
  std::string version;
  std::size_t content_length{0};
};

/**
 * @brief Multipart upload handle returned by Client::StartUpload.
 *
 * Thread-compat: each handle is owned by one logical uploader; multiple parts
 * can be uploaded sequentially. UploadPart is synchronous and returns the
 * (part_number, etag) needed at Complete time.
 */
class MultipartUpload {
 public:
  ~MultipartUpload();
  MultipartUpload(const MultipartUpload&) = delete;
  MultipartUpload& operator=(const MultipartUpload&) = delete;
  MultipartUpload(MultipartUpload&&) noexcept;
  MultipartUpload& operator=(MultipartUpload&&) noexcept;

  [[nodiscard]] const std::string& upload_id() const noexcept;
  [[nodiscard]] std::size_t max_part_size() const noexcept;

  /** @brief Sets the per-chunk checksum policy ("none" | "md5"). */
  void set_checksum_policy(std::string policy);

  [[nodiscard]] Result<TransferOutcome>
    UploadPart(std::uint32_t part_number, std::uint64_t object_offset,
               ConstBufferView buffer);

  struct PartSpec {
    std::uint32_t   part_number{0};
    std::uint64_t   object_offset{0};
    ConstBufferView buffer{};
  };

  /**
   * @brief Uploads multiple parts in parallel. On the first failure the
   *        remaining in-flight uploads still drain; the first error is
   *        returned. Each part must reference a distinct device buffer.
   */
  [[nodiscard]] Result<std::vector<TransferOutcome>>
    UploadParts(const std::vector<PartSpec>& parts, std::size_t concurrency);

  [[nodiscard]] Result<CompleteUploadResult> Complete();
  [[nodiscard]] Result<bool> Abort();

 private:
  friend class Client;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit MultipartUpload(std::unique_ptr<Impl> impl);
};

class Client {
 public:
  explicit Client(ClientOptions options);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

  [[nodiscard]] Result<bool> Initialize();
  void Shutdown();
  [[nodiscard]] bool initialized() const;
  [[nodiscard]] const PlatformCapabilities& capabilities() const;

  [[nodiscard]] Result<ObjectMetadata> HeadObject(const ObjectId& object) const;
  [[nodiscard]] Result<TransferOutcome> GetObject(const RequestOptions& request,
                                                  MutableBufferView buffer) const;
  [[nodiscard]] Result<TransferOutcome> PutObject(const RequestOptions& request,
                                                  ConstBufferView buffer) const;

  /*
   * 异步版本：把同步实现 Submit 到客户端共享线程池，立即返回 future。
   * 调用方必须保证 buffer 在 future 完成前保持有效（视图传值，但底层指针仍是用户的）。
   * 阶段 A 仅是 sync→future 封装，QP 资源仍按调用现场建/拆；C5 切到 CQ 完成
   * 线程后，本组接口保持不变。
   */
  [[nodiscard]] std::future<Result<ObjectMetadata>>
    HeadObjectAsync(const ObjectId& object) const;
  [[nodiscard]] std::future<Result<TransferOutcome>>
    GetObjectAsync(const RequestOptions& request, MutableBufferView buffer) const;
  [[nodiscard]] std::future<Result<TransferOutcome>>
    PutObjectAsync(const RequestOptions& request, ConstBufferView buffer) const;

  [[nodiscard]] Result<MultipartUpload>
    StartUpload(const ObjectId& object, std::size_t expected_total_size = 0,
                const std::string& idempotency_key = {});

  /**
   * @brief 预先把 GPU device buffer 注册到 cuObj descriptor 表。可选优化。
   *
   * GDS 通路下，每次 PUT/GET 之前 buffer 必须已经通过 cuMemObjGetDescriptor
   * 注册（pin 到 BAR1）。不调本接口时 SDK 会在第一次传输时 lazy 注册，但
   * 注册是毫秒级 syscall（nvidia_p2p_get_pages），落到热路径会抖动。
   * 推荐：cudaMalloc 之后立即 RegisterDeviceBuffer，cudaFree 之前
   * UnregisterDeviceBuffer。idempotent。
   *
   * 非 GDS 通路（HTTP / RDMA）下本调用 no-op，返回 success。
   */
  [[nodiscard]] Result<bool> RegisterDeviceBuffer(void* ptr, std::size_t size);

  /**
   * @brief 反注册 device buffer。**必须在 cudaFree(ptr) 之前调用**，否则
   * nvidia-fs 可能在内核侧死锁（NVIDIA 文档明确警告）。idempotent；从未
   * 注册或已反注册过的 ptr 也返回 success。
   */
  [[nodiscard]] Result<bool> UnregisterDeviceBuffer(void* ptr);

 private:
  std::unique_ptr<ClientCore> core_;
};

}  // namespace us3_turbo_access::client
