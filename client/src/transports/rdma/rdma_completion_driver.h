#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

struct ibv_cq;

namespace us3_turbo_access::client {

/**
 * 一次 RDMA WRITE 提交的完成结果（status / wr_id）。
 * status==IBV_WC_SUCCESS 视为成功；其他码透传给上层做错误判定。
 */
struct RdmaCompletion {
  int           status{0};   // ibv_wc.status
  std::uint64_t wr_id{0};
};

/**
 * 客户端 RDMA 完成线程：对所有 attach 进来的 CQ 做轮询，把 WC 通过 InflightMap
 * 派发到 promise。Worker 提交 WRITE 后只需 wait 对应 future。
 *
 * 协议：
 *   - 每条连接在 CQ 建好后 Attach(cq) 拿 conn_id。
 *   - PostWrite 前 RegisterInflight(conn_id, wr_id) → future。
 *   - 销毁该连接的 CQ 之前必须先 Detach(conn_id)；Detach 同步等待 driver loop
 *     跨过下一轮 polling，确保 driver 不会再触摸该 CQ。
 *   - Stop 后所有 inflight 都会被 set_exception，避免 future 永远悬挂。
 */
class RdmaCompletionDriver {
 public:
  RdmaCompletionDriver();
  ~RdmaCompletionDriver();

  RdmaCompletionDriver(const RdmaCompletionDriver&) = delete;
  RdmaCompletionDriver& operator=(const RdmaCompletionDriver&) = delete;

  /** 启动 driver thread（幂等）。*/
  void Start();
  /** 停止 driver thread（幂等，析构会自动调）。*/
  void Stop();

  /** 把一个 CQ 加入轮询列表，返回 conn_id。*/
  [[nodiscard]] std::uint64_t Attach(ibv_cq* cq);

  /**
   * 从轮询列表移除并同步等待 driver loop 跨过下一轮，确保 driver 持有的
   * snapshot 不再引用该 CQ。返回后调用方可以安全 ibv_destroy_cq。
   * 残留 inflight 通过 set_exception 收尾。
   */
  void Detach(std::uint64_t conn_id);

  /** PostWrite 之前调；返回 future 等完成。wr_id 由调用方分配且对本 conn 唯一。*/
  [[nodiscard]] std::future<RdmaCompletion>
    RegisterInflight(std::uint64_t conn_id, std::uint64_t wr_id);

 private:
  void Loop();
  void WaitEpochAdvance(std::uint64_t at_least);

  struct ConnSlot {
    ibv_cq* cq{nullptr};
    std::unordered_map<std::uint64_t, std::promise<RdmaCompletion>> inflight;
  };

  std::mutex                                            mu_;
  std::unordered_map<std::uint64_t, ConnSlot>           conns_;
  std::uint64_t                                         next_conn_id_{1};
  std::atomic<bool>                                     stop_{false};
  std::thread                                           thread_;
  bool                                                  started_{false};

  // epoch 在 Loop 每完成一轮 polling 后递增；Detach 等到 epoch 至少前进 2 才返回，
  // 保证 driver 上一轮拿走的 (conn_id, cq) snapshot 已不再被使用。
  std::atomic<std::uint64_t>                            epoch_{0};
  std::condition_variable                               epoch_cv_;
};

}  // namespace us3_turbo_access::client
