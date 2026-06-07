#pragma once

#include <cstddef>
#include <utility>

#include "us3_turbo_access/client/result.h"
#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

/**
 * @brief RAII wrapper for a pinned host buffer used by the RDMA transport.
 *
 * The native RDMA transport requires upload/download buffers to reside in
 * physically pinned (non-pageable) host pages. This helper allocates the
 * buffer through cudaHostAlloc, which provides genuine pinned memory even
 * on hosts without a discrete GPU (cudart is already a hard dependency).
 *
 * Callers that already own a pinned buffer (e.g. via mlock) may instead
 * pass a ConstBufferView with BufferType::kHostPinned directly to
 * Client::PutObject; this class is just a convenience.
 *
 * Ownership is move-only; the destructor returns the buffer to cudart.
 */
class [[deprecated(
    "PinnedBuffer uses cudaHostAlloc and was designed for the verbs RDMA path "
    "(now removed). For UCX path use aligned_alloc+mlock with BufferType::kHostRegular.")]]
PinnedBuffer {
 public:
  /**
   * @brief Allocates a pinned host buffer of @p size bytes.
   *
   * @return PinnedBuffer wrapped in Result on success; Failure carries the
   *         underlying CUDA error message.
   */
  [[nodiscard]] static Result<PinnedBuffer> Allocate(std::size_t size);

  PinnedBuffer() = default;
  ~PinnedBuffer();

  PinnedBuffer(const PinnedBuffer&) = delete;
  PinnedBuffer& operator=(const PinnedBuffer&) = delete;
  PinnedBuffer(PinnedBuffer&& other) noexcept;
  PinnedBuffer& operator=(PinnedBuffer&& other) noexcept;

  /** @brief Returns the raw buffer pointer; nullptr if not allocated. */
  [[nodiscard]] void*       data() noexcept       { return data_; }
  /** @brief Const overload of data(). */
  [[nodiscard]] const void* data() const noexcept { return data_; }
  /** @brief Returns the allocation size in bytes; 0 if not allocated. */
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  /** @brief Returns true when the buffer is allocated. */
  [[nodiscard]] bool        ok()   const noexcept { return data_ != nullptr; }

  /** @brief Returns a read-only view suitable for PutObject. */
  [[nodiscard]] ConstBufferView view() const noexcept {
    return ConstBufferView{.data = data_,
                            .size = size_,
                            .type = BufferType::kHostPinned};
  }

  /** @brief Returns a mutable view suitable for GetObject. */
  [[nodiscard]] MutableBufferView mutable_view() noexcept {
    return MutableBufferView{.data = data_,
                              .size = size_,
                              .type = BufferType::kHostPinned};
  }

  /**
   * @brief Releases the buffer ahead of destruction.
   *
   * Rarely needed; the destructor performs the same cleanup. Safe to call
   * multiple times; subsequent calls are no-ops.
   */
  void Reset() noexcept;

 private:
  PinnedBuffer(void* data, std::size_t size) noexcept
      : data_(data), size_(size) {}

  void*       data_{nullptr};
  std::size_t size_{0};
};

}  // namespace us3_turbo_access::client
