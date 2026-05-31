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

// 把 DataPath enum 映射为 array 索引（0/1/2）。
// 取值范围必须与 OpenSession 等枚举顺序一致：kHttpTcp=0, kNativeRdma=1, kGdsCuObject=2。
int PathIndex(DataPath p) {
  switch (p) {
    case DataPath::kHttpTcp:     return 0;
    case DataPath::kNativeRdma:  return 1;
    case DataPath::kGdsCuObject: return 2;
  }
  return 0;
}

// 给 path 子计数器拼 suffix。
std::string PathSuffix(DataPath p) {
  switch (p) {
    case DataPath::kHttpTcp:     return "_http_tcp";
    case DataPath::kNativeRdma:  return "_native_rdma";
    case DataPath::kGdsCuObject: return "_gds_cuobject";
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
  for (DataPath p : {DataPath::kHttpTcp, DataPath::kNativeRdma, DataPath::kGdsCuObject}) {
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
                                            std::optional<DataPath> data_path)
    : op_(op), bytes_(bytes), start_us_(NowUs()), data_path_(data_path) {}

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
