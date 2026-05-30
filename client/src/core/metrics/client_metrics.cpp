#include "client/src/core/metrics/client_metrics.h"

#include <chrono>

namespace us3_turbo_access::client {

namespace {

std::int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

ClientMetrics::ClientMetrics()
    : put_total("us3_client_put_total"),
      put_fail_total("us3_client_put_fail_total"),
      put_bytes("us3_client_put_bytes"),
      put_latency_us("us3_client_put_latency_us"),
      get_total("us3_client_get_total"),
      get_fail_total("us3_client_get_fail_total"),
      get_bytes("us3_client_get_bytes"),
      get_latency_us("us3_client_get_latency_us"),
      head_total("us3_client_head_total"),
      head_fail_total("us3_client_head_fail_total"),
      head_latency_us("us3_client_head_latency_us"),
      upload_part_total("us3_client_upload_part_total"),
      upload_part_fail_total("us3_client_upload_part_fail_total"),
      upload_part_bytes("us3_client_upload_part_bytes"),
      upload_part_latency_us("us3_client_upload_part_latency_us"),
      retry_total("us3_client_retry_total") {}

ClientMetrics& ClientMetrics::Instance() {
  static ClientMetrics m;
  return m;
}

ScopedTransferMetric::ScopedTransferMetric(Op op, std::int64_t bytes)
    : op_(op), bytes_(bytes), start_us_(NowUs()) {}

ScopedTransferMetric::~ScopedTransferMetric() {
  const auto elapsed_us = NowUs() - start_us_;
  auto& m = ClientMetrics::Instance();
  switch (op_) {
    case Op::kPut:
      m.put_latency_us << elapsed_us;
      if (success_) {
        m.put_total << 1;
        m.put_bytes << bytes_;
      } else {
        m.put_fail_total << 1;
      }
      break;
    case Op::kGet:
      m.get_latency_us << elapsed_us;
      if (success_) {
        m.get_total << 1;
        m.get_bytes << bytes_;
      } else {
        m.get_fail_total << 1;
      }
      break;
    case Op::kHead:
      m.head_latency_us << elapsed_us;
      if (success_) {
        m.head_total << 1;
      } else {
        m.head_fail_total << 1;
      }
      break;
    case Op::kUploadPart:
      m.upload_part_latency_us << elapsed_us;
      if (success_) {
        m.upload_part_total << 1;
        m.upload_part_bytes << bytes_;
      } else {
        m.upload_part_fail_total << 1;
      }
      break;
  }
}

}  // namespace us3_turbo_access::client
