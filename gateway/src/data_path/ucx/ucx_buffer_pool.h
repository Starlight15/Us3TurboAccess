#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include <ucp/api/ucp.h>

namespace us3_turbo_access::gateway::data_path::ucx {

/**
 * 已注册 MR 的 buffer slot。
 * pool 持有所有权；Acquire 借出，Return 归还，Drain/析构时 unmap+free。
 */
struct RegisteredBuffer {
  void*                     buf{nullptr};
  std::size_t               capacity{0};
  ucp_mem_h                 memh{nullptr};
  std::uint64_t             gw_raddr{0};
  std::vector<std::uint8_t> packed_rkey;
};

/**
 * 已注册 MR 的 buffer 复用池。
 *
 * 每个 slot 持有 {buf, memh, gw_raddr, packed_rkey}，Acquire 直接返回完整
 * slot（无需重新 ucp_mem_map + ucp_rkey_pack），Return 时 MR 保持注册状态，
 * 彻底消除每次 PrepareTransfer 的内核 pin+NIC 注册开销（~1-2ms/次）。
 *
 * 线程安全：mutex 保护，brpc RPC 线程并发 Acquire/Return 安全。
 */
class UcxBufferPool {
 public:
  UcxBufferPool(ucp_context_h ctx, std::size_t max_idle)
      : ctx_(ctx), max_idle_(max_idle) {}

  ~UcxBufferPool() { Drain(); }

  UcxBufferPool(const UcxBufferPool&) = delete;
  UcxBufferPool& operator=(const UcxBufferPool&) = delete;

  /**
   * 从池中取出 capacity >= requested_size 的 slot；无合适 slot 返回 nullptr。
   * 调用方负责在 nullptr 时自行分配+注册。
   */
  [[nodiscard]] RegisteredBuffer* Acquire(std::size_t requested_size);

  /**
   * 归还 slot（MR 保持注册状态）；超出 max_idle 时 unmap+free。
   */
  void Return(RegisteredBuffer* slot) noexcept;

  /** Stop 时调用，unmap 并释放所有缓存 slot。 */
  void Drain() noexcept;

 private:
  static constexpr std::size_t kPageSize = 4096;

  ucp_context_h                 ctx_;
  std::size_t                   max_idle_;
  std::mutex                    mu_;
  std::deque<RegisteredBuffer*> pool_;
};

}  // namespace us3_turbo_access::gateway::data_path::ucx
