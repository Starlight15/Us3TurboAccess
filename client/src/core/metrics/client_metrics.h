#pragma once

#include <cstdint>

#include <bvar/bvar.h>

#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

/**
 * 进程级 client 指标聚合，使用 brpc bvar。
 *
 * - 三通路（HTTP / RDMA / GDS）的 PUT / GET / HEAD / Multipart 共享同一组指标，
 *   按 DataPath 拆 sub-counter；本期先把"全局合计"打通，按 path 维度的
 *   细分通过日志/外部聚合即可。
 * - 应用进程内单实例：Metrics::Instance()。bvar 是全局 expose，多 Client
 *   实例的统计会自然聚合，本期可接受。
 * - 启动 brpc Server 时 bvar 自动暴露到 /vars；应用也可以直接读字段。
 *
 * 命名：us3_client_<op>_<metric>，与 gateway 端 us3_gateway_* 风格平行。
 */
class ClientMetrics {
 public:
  // PUT
  bvar::Adder<std::int64_t>     put_total;
  bvar::Adder<std::int64_t>     put_fail_total;
  bvar::Adder<std::int64_t>     put_bytes;
  bvar::LatencyRecorder         put_latency_us;

  // GET
  bvar::Adder<std::int64_t>     get_total;
  bvar::Adder<std::int64_t>     get_fail_total;
  bvar::Adder<std::int64_t>     get_bytes;
  bvar::LatencyRecorder         get_latency_us;

  // HEAD
  bvar::Adder<std::int64_t>     head_total;
  bvar::Adder<std::int64_t>     head_fail_total;
  bvar::LatencyRecorder         head_latency_us;

  // Multipart
  bvar::Adder<std::int64_t>     upload_part_total;
  bvar::Adder<std::int64_t>     upload_part_fail_total;
  bvar::Adder<std::int64_t>     upload_part_bytes;
  bvar::LatencyRecorder         upload_part_latency_us;

  // 重试
  bvar::Adder<std::int64_t>     retry_total;  // 累计触发的重试次数（不含首次）

  static ClientMetrics& Instance();

 private:
  ClientMetrics();
};

/**
 * RAII：构造时记起始时间；析构时根据 success 累加 *_total / *_fail_total，
 * 同时灌 latency 直方图。
 */
class ScopedTransferMetric {
 public:
  enum class Op { kPut, kGet, kHead, kUploadPart };

  ScopedTransferMetric(Op op, std::int64_t bytes = 0);
  ~ScopedTransferMetric();

  ScopedTransferMetric(const ScopedTransferMetric&) = delete;
  ScopedTransferMetric& operator=(const ScopedTransferMetric&) = delete;

  /** 标记本次操作成功；不调表示失败。 */
  void MarkSuccess() noexcept { success_ = true; }

  /** 更新实际传输字节（成功时计入 *_bytes）。 */
  void SetBytes(std::int64_t bytes) noexcept { bytes_ = bytes; }

 private:
  Op           op_;
  bool         success_{false};
  std::int64_t bytes_{0};
  std::int64_t start_us_;
};

}  // namespace us3_turbo_access::client
