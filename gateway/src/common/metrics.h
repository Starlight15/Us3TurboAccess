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

}  // namespace us3_turbo_access::gateway::common
