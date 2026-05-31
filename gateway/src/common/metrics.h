#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <bvar/bvar.h>

namespace us3_turbo_access::gateway::common {

/**
 * @brief 全局 metrics 句柄；进程级单例，bvar 自动接 brpc 的 /vars。
 *
 * 用法：metrics().open_session_total << 1;  // counter +1
 *      metrics().gds_put_latency_us << elapsed_us;  // latency recorder
 *
 * 仅在 gateway 进程使用；client 端复用 brpc client 内置的 bvar。
 */
struct Metrics {
  // Session lifecycle
  bvar::Adder<std::int64_t>     open_session_total;
  bvar::Adder<std::int64_t>     open_session_fail_total;
  bvar::LatencyRecorder         open_session_latency_us;
  bvar::Adder<std::int64_t>     sessions_evicted_total;

  // GDS data plane
  bvar::Adder<std::int64_t>     gds_put_total;
  bvar::Adder<std::int64_t>     gds_get_total;
  bvar::Adder<std::int64_t>     gds_put_fail_total;
  bvar::Adder<std::int64_t>     gds_get_fail_total;
  bvar::Adder<std::int64_t>     gds_put_bytes;
  bvar::Adder<std::int64_t>     gds_get_bytes;
  bvar::LatencyRecorder         gds_put_latency_us;
  bvar::LatencyRecorder         gds_get_latency_us;

  // HTTP data plane（PUT / GET / HEAD 单对象路径；multipart 路径待 HTTP-multipart review 后补齐）
  bvar::Adder<std::int64_t>     http_put_total;
  bvar::Adder<std::int64_t>     http_get_total;
  bvar::Adder<std::int64_t>     http_head_total;
  bvar::Adder<std::int64_t>     http_put_fail_total;
  bvar::Adder<std::int64_t>     http_get_fail_total;
  bvar::Adder<std::int64_t>     http_head_fail_total;
  bvar::Adder<std::int64_t>     http_put_bytes;
  bvar::Adder<std::int64_t>     http_get_bytes;
  bvar::LatencyRecorder         http_put_latency_us;
  bvar::LatencyRecorder         http_get_latency_us;
  bvar::LatencyRecorder         http_head_latency_us;
  bvar::Adder<std::int64_t>     http_rejected_total;  // 503 due to max_concurrency

  // HTTP inflight：与 GDS/RDMA 走 SessionStore 的"可观察"对齐。
  // HTTP PUT/GET/HEAD 不进 SessionStore（一次 RPC 完成），用 inflight gauge
  // 在 frontend 入口/出口 RAII 进/出，让运维看到 in-progress 请求数。
  // 失败请求也走 RAII，inflight_aborted_total 单独计数（成功 RAII 不计数）。
  bvar::Adder<std::int64_t>     http_put_inflight;
  bvar::Adder<std::int64_t>     http_get_inflight;
  bvar::Adder<std::int64_t>     http_head_inflight;
  bvar::Adder<std::int64_t>     http_put_inflight_aborted_total;
  bvar::Adder<std::int64_t>     http_get_inflight_aborted_total;
  bvar::Adder<std::int64_t>     http_head_inflight_aborted_total;

  // Backend
  bvar::Adder<std::int64_t>     backend_write_total;
  bvar::Adder<std::int64_t>     backend_read_total;
  bvar::Adder<std::int64_t>     backend_write_bytes;
  bvar::Adder<std::int64_t>     backend_read_bytes;

  // Multipart upload
  bvar::Adder<std::int64_t>     multipart_start_total;
  bvar::Adder<std::int64_t>     multipart_complete_total;
  bvar::Adder<std::int64_t>     multipart_abort_total;
  bvar::Adder<std::int64_t>     multipart_fail_total;
  bvar::Adder<std::int64_t>     multipart_part_bytes;
  bvar::Adder<std::int64_t>     multipart_swept_total;
  bvar::LatencyRecorder         multipart_complete_latency_us;

  Metrics();
};

[[nodiscard]] Metrics& metrics();

/**
 * @brief RAII：在 dtor 时把 elapsed (us) 灌进给定 LatencyRecorder。
 */
class ScopedLatency {
 public:
  explicit ScopedLatency(bvar::LatencyRecorder& recorder);
  ~ScopedLatency();

  ScopedLatency(const ScopedLatency&) = delete;
  ScopedLatency& operator=(const ScopedLatency&) = delete;

 private:
  bvar::LatencyRecorder& recorder_;
  std::int64_t           start_us_;
};

/**
 * @brief RAII：构造时 inflight +1，析构时 inflight -1；MarkAborted 累计 abort 计数。
 *
 * 用法（HttpFrontend 入口）：
 *   common::ScopedHttpInflight g(common::metrics().http_put_inflight,
 *                                common::metrics().http_put_inflight_aborted_total);
 *   if (...failed) { g.MarkAborted(); return; }
 */
class ScopedHttpInflight {
 public:
  ScopedHttpInflight(bvar::Adder<std::int64_t>& inflight,
                     bvar::Adder<std::int64_t>& aborted);
  ~ScopedHttpInflight();

  ScopedHttpInflight(const ScopedHttpInflight&) = delete;
  ScopedHttpInflight& operator=(const ScopedHttpInflight&) = delete;

  void MarkAborted() noexcept { aborted_ = true; }

 private:
  bvar::Adder<std::int64_t>& inflight_;
  bvar::Adder<std::int64_t>& aborted_counter_;
  bool                       aborted_{false};
};

}  // namespace us3_turbo_access::gateway::common
