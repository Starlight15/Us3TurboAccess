#pragma once

#include <cstddef>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

struct ibv_mr;
struct ibv_pd;

namespace us3_turbo_access::client {

/**
 * RDMA host buffer 的 RAII 注册句柄。析构自动 ibv_dereg_mr。
 */
class MrLease {
 public:
  MrLease() = default;
  MrLease(ibv_mr* mr) noexcept : mr_(mr) {}
  ~MrLease();

  MrLease(const MrLease&) = delete;
  MrLease& operator=(const MrLease&) = delete;
  MrLease(MrLease&& o) noexcept : mr_(o.mr_) { o.mr_ = nullptr; }
  MrLease& operator=(MrLease&& o) noexcept;

  [[nodiscard]] bool        ok()    const noexcept { return mr_ != nullptr; }
  [[nodiscard]] void*       addr()  const noexcept;
  [[nodiscard]] std::size_t size()  const noexcept;
  [[nodiscard]] std::uint32_t lkey() const noexcept;
  [[nodiscard]] std::uint32_t rkey() const noexcept;
  [[nodiscard]] ibv_mr*      raw()  const noexcept { return mr_; }

 private:
  ibv_mr* mr_{nullptr};
};

/**
 * 注册 RDMA host buffer：强制 kHostPinned；调 ibv_reg_mr；
 * 返回 MrLease（RAII 自动 dereg）。GPU 内存 / regular host 内存一律拒。
 */
class HostMemoryRegistry {
 public:
  explicit HostMemoryRegistry(ibv_pd* pd) noexcept : pd_(pd) {}

  [[nodiscard]] Result<MrLease> Register(ConstBufferView buffer) const;
  [[nodiscard]] Result<MrLease> Register(MutableBufferView buffer) const;

 private:
  ibv_pd* pd_{nullptr};
};

}  // namespace us3_turbo_access::client
