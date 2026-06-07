#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <ucp/api/ucp.h>

#include "client/src/transports/ucx/ucx_worker.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

/** ep + 已解包的 rkey，作为整体从 pool 借出/归还。 */
struct EpSlot {
  ucp_ep_h   ep{nullptr};
  ucp_rkey_h rkey{nullptr};  // 对应当前 gateway slot 的 packed_rkey 解包结果
};

/**
 * UCX endpoint + rkey 复用池。
 *
 * 每个 slot 持有 {ep, rkey_h}，Acquire 直接返回可用 slot，无需
 * ucp_ep_create 握手（~8ms）或 ucp_ep_rkey_unpack（~0.1ms）。
 *
 * 注意：rkey 与 gateway 的 MR 绑定。若 gateway 重启导致 MR 变化，
 * 旧 rkey 失效——此时 ucp_put_nbx 会报错，调用方 Discard 并重建即可。
 *
 * 线程安全：内部 mutex 保护。
 */
class UcxEndpointPool {
 public:
  explicit UcxEndpointPool(UcxWorker& worker,
                            std::size_t max_idle_per_endpoint = 8)
      : worker_(worker), max_idle_(max_idle_per_endpoint) {}

  ~UcxEndpointPool();

  UcxEndpointPool(const UcxEndpointPool&) = delete;
  UcxEndpointPool& operator=(const UcxEndpointPool&) = delete;

  /**
   * 借出 slot（ep+rkey）；池中无空闲时新建 ep（rkey=nullptr，调用方 unpack 后 Return）。
   * packed_rkey 用于首次 unpack；若 slot 已有 rkey，传入的 packed_rkey 被忽略。
   */
  [[nodiscard]] Result<EpSlot> Acquire(
      const std::string& host, uint16_t port,
      const std::vector<std::uint8_t>& packed_rkey);

  /** 归还 slot（成功时调用）；超出 max_idle 时销毁。 */
  void Return(const std::string& host, uint16_t port, EpSlot slot);

  /** 出错时丢弃 slot（不归还）。 */
  void Discard(EpSlot slot) noexcept;

 private:
  static std::string Key(const std::string& host, uint16_t port) {
    return host + ":" + std::to_string(port);
  }

  UcxWorker&                                                  worker_;
  std::size_t                                                 max_idle_;
  std::mutex                                                  mu_;
  std::unordered_map<std::string, std::deque<EpSlot>>         idle_;
};

}  // namespace us3_turbo_access::client
