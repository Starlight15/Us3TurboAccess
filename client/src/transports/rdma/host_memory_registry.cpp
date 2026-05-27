#include "client/src/transports/rdma/host_memory_registry.h"

#include <infiniband/verbs.h>

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

namespace {

template <typename BufferView>
[[nodiscard]] Result<MrLease> RegisterImpl(ibv_pd* pd, BufferView buffer) {
  if (pd == nullptr) {
    return Result<MrLease>::Failure(MakeUnsupportedPath(
        DataPath::kNativeRdma, "RDMA PD 未初始化（RdmaResources 未启动？）"));
  }
  if (buffer.type != BufferType::kHostPinned) {
    return Result<MrLease>::Failure(MakeUnsupportedPath(
        DataPath::kNativeRdma,
        "RDMA 路径只接受 kHostPinned；用 PinnedBuffer::Allocate 或自行 mlock"));
  }
  if (buffer.data == nullptr || buffer.size == 0) {
    return Result<MrLease>::Failure(
        MakeInvalidArgument("buffer 为空"));
  }
  auto* mr = ibv_reg_mr(
      pd, const_cast<void*>(buffer.data), buffer.size,
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
  if (mr == nullptr) {
    return Result<MrLease>::Failure(MakeUnsupportedPath(
        DataPath::kNativeRdma,
        "ibv_reg_mr 失败（页未真正 pinned？或 NIC 资源不足）"));
  }
  return Result<MrLease>::Success(MrLease(mr));
}

}  // namespace

MrLease::~MrLease() {
  if (mr_ != nullptr) {
    (void)ibv_dereg_mr(mr_);
    mr_ = nullptr;
  }
}

MrLease& MrLease::operator=(MrLease&& o) noexcept {
  if (this != &o) {
    if (mr_ != nullptr) {
      (void)ibv_dereg_mr(mr_);
    }
    mr_ = o.mr_;
    o.mr_ = nullptr;
  }
  return *this;
}

void* MrLease::addr() const noexcept {
  return mr_ != nullptr ? mr_->addr : nullptr;
}
std::size_t MrLease::size() const noexcept {
  return mr_ != nullptr ? mr_->length : 0;
}
std::uint32_t MrLease::lkey() const noexcept {
  return mr_ != nullptr ? mr_->lkey : 0;
}
std::uint32_t MrLease::rkey() const noexcept {
  return mr_ != nullptr ? mr_->rkey : 0;
}

Result<MrLease> HostMemoryRegistry::Register(ConstBufferView buffer) const {
  return RegisterImpl(pd_, buffer);
}

Result<MrLease> HostMemoryRegistry::Register(MutableBufferView buffer) const {
  return RegisterImpl(pd_, buffer);
}

}  // namespace us3_turbo_access::client
