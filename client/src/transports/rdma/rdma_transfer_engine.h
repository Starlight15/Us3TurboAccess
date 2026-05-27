#pragma once

#include <cstdint>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

struct ibv_qp;

namespace us3_turbo_access::client {

class MrLease;

/**
 * 客户端 RDMA WRITE 提交接口。
 *
 * 拆成 PostWrite + 完成等待两段：PostWrite 只把 WR 灌到 QP，完成事件由
 * [[rdma_completion_driver]] 异步触发。这样 worker 不再 spin poll CQ，多个
 * inflight 共享 driver。
 */
class RdmaTransferEngine {
 public:
  /**
   * 提交一个 RDMA WRITE（IBV_WR_RDMA_WRITE，IBV_SEND_SIGNALED）。
   * 调用方在 PostWrite 前先 RegisterInflight(wr_id) 拿 future。
   * length 必须 <= src.size()，并且 <= NIC 单 WR 上限。
   */
  [[nodiscard]] static Result<bool>
    PostWrite(ibv_qp* qp, const MrLease& src,
              std::uint64_t length, std::uint64_t raddr,
              std::uint32_t rkey, std::uint64_t wr_id);
};

}  // namespace us3_turbo_access::client
