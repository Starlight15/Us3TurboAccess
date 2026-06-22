#include "client/src/core/metrics/client_metrics.h"

#include <chrono>
#include <string>

namespace us3_turbo_access::client {

namespace {

std::int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 把 DataFlow enum 映射为 array 索引（0/1）。
// 取值范围：GPUDirect=0, CPUDirect=1。
int PathIndex(DataFlow p) {
  switch (p) {
    case DataFlow::GPUDirect: return 0;
    case DataFlow::CPUDirect:  return 1;
  }
  return 0;
}

// 给 path 子计数器拼 suffix。
std::string PathSuffix(DataFlow p) {
  switch (p) {
    case DataFlow::GPUDirect: return "_gds_cuobject";
    case DataFlow::CPUDirect:  return "_native_rdma";
  }
  return "_unknown";
}

}  // namespace

ClientMetrics::OpCounters::OpCounters(const char* prefix)
    : total((std::string(prefix) + "_total").c_str()),
      fail_total((std::string(prefix) + "_fail_total").c_str()),
      bytes((std::string(prefix) + "_bytes").c_str()),
      latency_us((std::string(prefix) + "_latency_us").c_str()) {}

ClientMetrics::ClientMetrics()
    : put("us3_client_put"),
      get("us3_client_get"),
      head("us3_client_head"),
      upload_part("us3_client_upload_part"),
      retry_total("us3_client_retry_total") {
  // per-path 子计数器
  for (DataFlow p : {DataFlow::GPUDirect, DataFlow::CPUDirect}) {
    const auto idx = PathIndex(p);
    const auto suf = PathSuffix(p);
    put_by_path[idx] = std::make_unique<OpCounters>(
        ("us3_client_put" + suf).c_str());
    get_by_path[idx] = std::make_unique<OpCounters>(
        ("us3_client_get" + suf).c_str());
    head_by_path[idx] = std::make_unique<OpCounters>(
        ("us3_client_head" + suf).c_str());
    upload_part_by_path[idx] = std::make_unique<OpCounters>(
        ("us3_client_upload_part" + suf).c_str());
  }
}

ClientMetrics& ClientMetrics::Instance() {
  static ClientMetrics m;
  return m;
}

ScopedTransferMetric::ScopedTransferMetric(Op op, std::int64_t bytes,
                                            std::optional<DataFlow> data_flow)
    : op_(op), bytes_(bytes), start_us_(NowUs()), data_path_(data_flow) {}

ScopedTransferMetric::~ScopedTransferMetric() {
  const auto elapsed_us = NowUs() - start_us_;
  auto& m = ClientMetrics::Instance();

  // 选全局 + per-path 双 OpCounters：per_path 可能为 nullopt（调用方没传 path）。
  ClientMetrics::OpCounters* global = nullptr;
  ClientMetrics::OpCounters* perpath = nullptr;
  switch (op_) {
    case Op::kPut:
      global = &m.put;
      if (data_path_) perpath = m.put_by_path[PathIndex(*data_path_)].get();
      break;
    case Op::kGet:
      global = &m.get;
      if (data_path_) perpath = m.get_by_path[PathIndex(*data_path_)].get();
      break;
    case Op::kHead:
      global = &m.head;
      if (data_path_) perpath = m.head_by_path[PathIndex(*data_path_)].get();
      break;
    case Op::kUploadPart:
      global = &m.upload_part;
      if (data_path_) perpath = m.upload_part_by_path[PathIndex(*data_path_)].get();
      break;
  }

  auto record = [&](ClientMetrics::OpCounters* c) {
    if (c == nullptr) return;
    c->latency_us << elapsed_us;
    if (success_) {
      c->total << 1;
      c->bytes << bytes_;
    } else {
      c->fail_total << 1;
    }
  };
  record(global);
  record(perpath);
}

}  // namespace us3_turbo_access::client
