#include "client/src/transports/rdma/rdma_transfer_engine.h"

#include <infiniband/verbs.h>

#include "client/src/core/common/errors.h"
#include "client/src/transports/rdma/host_memory_registry.h"

namespace us3_turbo_access::client {

/*
 * 提交一个 IBV_WR_RDMA_WRITE。本函数只 post，不等完成：
 *   - 完成由 RdmaCompletionDriver 派发到对应的 wr_id promise；
 *   - 调用方必须在 PostWrite 之前 RegisterInflight(wr_id)，否则 WC 会丢失。
 *
 * 单 sge 单 WR：本期不支持把一个 RDMA 消息拆成多片，length 上限由
 * gateway 的 max_msg_bytes 控制（已在上层 PrepareAndWrite 拒绝过超长）。
 * SIGNALED：保证 WC 一定上 CQ；如果未来要做无 signal 批量提交需要改这里。
 * 失败 retryable=true：通常是 send queue 满或瞬时拒绝，上层可重试。
 */
Result<bool> RdmaTransferEngine::PostWrite(ibv_qp* qp, const MrLease& src,
                                             std::uint64_t length,
                                             std::uint64_t raddr,
                                             std::uint32_t rkey,
                                             std::uint64_t wr_id) {
  if (qp == nullptr || !src.ok() || length == 0) {
    return Result<bool>::Failure(
        MakeInvalidArgument("RdmaTransferEngine::PostWrite 参数无效"));
  }
  if (length > src.size()) {
    return Result<bool>::Failure(MakeInvalidArgument(
        "RDMA WRITE length 超过 source MR size"));
  }

  // sg_list 用本地 MR 的 (addr, lkey)；length 取 buffer 实际字节数。
  ibv_sge sge{};
  sge.addr   = reinterpret_cast<std::uint64_t>(src.addr());
  sge.length = static_cast<std::uint32_t>(length);
  sge.lkey   = src.lkey();

  // wr_id 由调用方分配，driver 据此回路到 promise。
  ibv_send_wr wr{};
  wr.wr_id      = wr_id;
  wr.opcode     = IBV_WR_RDMA_WRITE;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.num_sge    = 1;
  wr.sg_list    = &sge;
  wr.wr.rdma.remote_addr = raddr;
  wr.wr.rdma.rkey        = rkey;

  ibv_send_wr* bad = nullptr;
  if (ibv_post_send(qp, &wr, &bad) != 0) {
    return Result<bool>::Failure(MakeTransportFailure(
        "ibv_post_send failed", DataPath::kNativeRdma, "", true));
  }
  return Result<bool>::Success(true);
}

}  // namespace us3_turbo_access::client
