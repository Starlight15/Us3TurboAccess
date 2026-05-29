#include "common/metrics.h"

#include <chrono>

namespace us3_turbo_access::gateway::common {

namespace {

std::int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

Metrics::Metrics()
    : open_session_total("gateway_open_session_total"),
      open_session_fail_total("gateway_open_session_fail_total"),
      open_session_latency_us("gateway_open_session_latency_us"),
      sessions_evicted_total("gateway_sessions_evicted_total"),
      gds_put_total("gateway_gds_put_total"),
      gds_get_total("gateway_gds_get_total"),
      gds_put_fail_total("gateway_gds_put_fail_total"),
      gds_get_fail_total("gateway_gds_get_fail_total"),
      gds_put_bytes("gateway_gds_put_bytes"),
      gds_get_bytes("gateway_gds_get_bytes"),
      gds_put_latency_us("gateway_gds_put_latency_us"),
      gds_get_latency_us("gateway_gds_get_latency_us"),
      http_put_total("gateway_http_put_total"),
      http_get_total("gateway_http_get_total"),
      http_head_total("gateway_http_head_total"),
      http_put_fail_total("gateway_http_put_fail_total"),
      http_get_fail_total("gateway_http_get_fail_total"),
      http_head_fail_total("gateway_http_head_fail_total"),
      http_put_bytes("gateway_http_put_bytes"),
      http_get_bytes("gateway_http_get_bytes"),
      http_put_latency_us("gateway_http_put_latency_us"),
      http_get_latency_us("gateway_http_get_latency_us"),
      http_head_latency_us("gateway_http_head_latency_us"),
      http_rejected_total("gateway_http_rejected_total"),
      backend_write_total("gateway_backend_write_total"),
      backend_read_total("gateway_backend_read_total"),
      backend_write_bytes("gateway_backend_write_bytes"),
      backend_read_bytes("gateway_backend_read_bytes"),
      multipart_start_total("gateway_multipart_start_total"),
      multipart_complete_total("gateway_multipart_complete_total"),
      multipart_abort_total("gateway_multipart_abort_total"),
      multipart_fail_total("gateway_multipart_fail_total"),
      multipart_part_bytes("gateway_multipart_part_bytes"),
      multipart_swept_total("gateway_multipart_swept_total"),
      multipart_complete_latency_us("gateway_multipart_complete_latency_us") {}

Metrics& metrics() {
  static Metrics instance;
  return instance;
}

ScopedLatency::ScopedLatency(bvar::LatencyRecorder& recorder)
    : recorder_(recorder), start_us_(NowUs()) {}

ScopedLatency::~ScopedLatency() {
  recorder_ << (NowUs() - start_us_);
}

}  // namespace us3_turbo_access::gateway::common
