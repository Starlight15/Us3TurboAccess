#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

#include <bvar/bvar.h>

#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

/**
 * 进程级 client 指标聚合，使用 brpc bvar。
 *
 * 全局合计 + per-path 拆分两套并存（C.4）：
 *   us3_client_<op>_<metric>                 ← 全局合计（向后兼容）
 *   us3_client_<op>_<metric>_<path>          ← 按通路细分
 *      <path> 取自 ToString(DataPath): "http-tcp" / "native-rdma" / "gds-cuobject"
 *
 * 这样运维既能看总量，也能看哪条通路慢/失败多。
 *
 * - 应用进程内单实例：Metrics::Instance()。bvar 是全局 expose，多 Client
 *   实例的统计会自然聚合，本期可接受。
 * - 启动 brpc Server 时 bvar 自动暴露到 /vars；应用也可以直接读字段。
 */
class ClientMetrics {
 public:
  // 每个 op 的全局合计 + per-path 拆分
  struct OpCounters {
    bvar::Adder<std::int64_t>     total;
    bvar::Adder<std::int64_t>     fail_total;
    bvar::Adder<std::int64_t>     bytes;
    bvar::LatencyRecorder         latency_us;

    OpCounters(const char* prefix);
  };

  // PUT / GET / HEAD / UploadPart 共 4 个全局 + 各 3 个 per-path
  OpCounters put;
  OpCounters get;
  OpCounters head;
  OpCounters upload_part;

  // per-path：用 unique_ptr 是因为 OpCounters ctor 取 char* prefix，
  // 不能直接 array 默认构造；显式管理生命周期。
  std::array<std::unique_ptr<OpCounters>, 3> put_by_path;          // [HttpTcp, NativeRdma, GdsCuObject]
  std::array<std::unique_ptr<OpCounters>, 3> get_by_path;
  std::array<std::unique_ptr<OpCounters>, 3> head_by_path;
  std::array<std::unique_ptr<OpCounters>, 3> upload_part_by_path;

  // 重试
  bvar::Adder<std::int64_t>     retry_total;  // 累计触发的重试次数（不含首次）

  static ClientMetrics& Instance();

 private:
  ClientMetrics();
};

/**
 * RAII：构造时记起始时间；析构时根据 success 累加 *_total / *_fail_total，
 * 同时灌 latency 直方图。
 *
 * data_path 可选：传了就同时记 per-path 拆分（C.4）；不传只记全局。
 */
class ScopedTransferMetric {
 public:
  enum class Op { kPut, kGet, kHead, kUploadPart };

  ScopedTransferMetric(Op op, std::int64_t bytes = 0,
                        std::optional<DataPath> data_path = std::nullopt);
  ~ScopedTransferMetric();

  ScopedTransferMetric(const ScopedTransferMetric&) = delete;
  ScopedTransferMetric& operator=(const ScopedTransferMetric&) = delete;

  /** 标记本次操作成功；不调表示失败。 */
  void MarkSuccess() noexcept { success_ = true; }

  /** 更新实际传输字节（成功时计入 *_bytes）。 */
  void SetBytes(std::int64_t bytes) noexcept { bytes_ = bytes; }

  /** 调用方拿到 outcome.selected_path 后追加 path（覆盖之前传入的）。 */
  void SetDataPath(DataPath p) noexcept { data_path_ = p; }

 private:
  Op                       op_;
  bool                     success_{false};
  std::int64_t             bytes_{0};
  std::int64_t             start_us_;
  std::optional<DataPath>  data_path_;
};

}  // namespace us3_turbo_access::client
