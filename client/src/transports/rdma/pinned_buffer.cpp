#include "us3_turbo_access/client/pinned_buffer.h"

#include <cuda_runtime.h>

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

Result<PinnedBuffer> PinnedBuffer::Allocate(std::size_t size) {
  if (size == 0) {
    return Result<PinnedBuffer>::Failure(
        MakeInvalidArgument("PinnedBuffer::Allocate size 不可为 0"));
  }
  void* ptr = nullptr;
  const auto rc = cudaHostAlloc(&ptr, size, cudaHostAllocDefault);
  if (rc != cudaSuccess || ptr == nullptr) {
    return Result<PinnedBuffer>::Failure(MakeInvalidArgument(
        std::string("cudaHostAlloc 失败: ") + cudaGetErrorString(rc)));
  }
  return Result<PinnedBuffer>::Success(PinnedBuffer(ptr, size));
}

PinnedBuffer::~PinnedBuffer() { Reset(); }

PinnedBuffer::PinnedBuffer(PinnedBuffer&& other) noexcept
    : data_(other.data_), size_(other.size_) {
  other.data_ = nullptr;
  other.size_ = 0;
}

PinnedBuffer& PinnedBuffer::operator=(PinnedBuffer&& other) noexcept {
  if (this != &other) {
    Reset();
    data_ = other.data_;
    size_ = other.size_;
    other.data_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

void PinnedBuffer::Reset() noexcept {
  if (data_ != nullptr) {
    (void)cudaFreeHost(data_);
    data_ = nullptr;
    size_ = 0;
  }
}

}  // namespace us3_turbo_access::client
