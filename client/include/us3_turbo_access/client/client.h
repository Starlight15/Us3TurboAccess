#pragma once

#include <cstdint>
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

  [[nodiscard]] Result<MultipartUpload>
    StartUpload(const ObjectId& object, std::size_t expected_total_size = 0,
                const std::string& idempotency_key = {});

 private:
  std::unique_ptr<ClientCore> core_;
};

}  // namespace us3_turbo_access::client
