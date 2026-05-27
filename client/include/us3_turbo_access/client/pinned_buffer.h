#pragma once

#include <cstddef>
#include <utility>

#include "us3_turbo_access/client/result.h"
#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

/**
 * Host pinned buffer 的 RAII 包装；释放在析构。
 *
 * Native RDMA 路径要求 buffer 落在 pinned 物理页（不被 swap 出去）。
 * 用户可以用本 helper 一次性分配 + 自动释放；也可以自带 mlock 的 buffer
 * 并把 BufferType 标成 kHostPinned 直接传入 PutObject。
 *
 * 实现选择：底层用 cudaHostAlloc（cudart 已链接），即便不用 GPU 也能拿到
 * 真正 pinned 的 host 页。需要时可改成 mlock。
 */
class PinnedBuffer {
 public:
  /** 分配一段 size 字节的 pinned host buffer。失败返回 Result::Failure。*/
  [[nodiscard]] static Result<PinnedBuffer> Allocate(std::size_t size);

  PinnedBuffer() = default;
  ~PinnedBuffer();

  PinnedBuffer(const PinnedBuffer&) = delete;
  PinnedBuffer& operator=(const PinnedBuffer&) = delete;
  PinnedBuffer(PinnedBuffer&& other) noexcept;
  PinnedBuffer& operator=(PinnedBuffer&& other) noexcept;

  [[nodiscard]] void*       data() noexcept       { return data_; }
  [[nodiscard]] const void* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool        ok()   const noexcept { return data_ != nullptr; }

  /** 给 PutObject 用：拼装成只读视图。*/
  [[nodiscard]] ConstBufferView view() const noexcept {
    return ConstBufferView{.data = data_,
                            .size = size_,
                            .type = BufferType::kHostPinned};
  }
  /** 给 GetObject 用：拼装成可写视图。*/
  [[nodiscard]] MutableBufferView mutable_view() noexcept {
    return MutableBufferView{.data = data_,
                              .size = size_,
                              .type = BufferType::kHostPinned};
  }

  /** 释放（手动）。析构会自动调用，调用方一般不需要 */
  void Reset() noexcept;

 private:
  PinnedBuffer(void* data, std::size_t size) noexcept
      : data_(data), size_(size) {}

  void*       data_{nullptr};
  std::size_t size_{0};
};

}  // namespace us3_turbo_access::client
