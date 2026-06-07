#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>

#include <ucp/api/ucp.h>

#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

/**
 * UCX endpoint 工具函数（RMA WRITE 模式）。
 *
 * CloseEpGraceful：优先 FLUSH 关闭（等待远端确认），超时后降级 FORCE。
 *
 * PutAndNotify：ucp_put_nbx 写数据到 gw_raddr，flush ep 确保 NIC 传输完成，
 *               再发 AM_WRITE_DONE（header = session_id）通知 gateway 数据已就绪。
 */
class UcxEndpointOps {
 public:
  // AM handler ID：与 gateway UcxExecutor::kAmIdWriteDone 保持一致
  static constexpr std::uint8_t kAmIdWriteDone = 0;

  static void CloseEpGraceful(
      ucp_worker_h worker, ucp_ep_h ep,
      std::chrono::milliseconds graceful_timeout = std::chrono::milliseconds(100)) noexcept;

  /**
   * RDMA WRITE + flush + AM_WRITE_DONE 三步串联异步操作。
   * 返回 future，caller 在 progress loop 中等待完成后发 CommitObject RPC。
   */
  [[nodiscard]] static std::future<ucs_status_t> PutAndNotify(
      ucp_worker_h worker, ucp_ep_h ep,
      const void* src_buf, std::size_t size,
      std::uint64_t gw_raddr, ucp_rkey_h gw_rkey,
      const std::string& session_id);
};

}  // namespace us3_turbo_access::client
