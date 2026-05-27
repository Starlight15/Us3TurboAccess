// 6 个 bench (rdma|gds|http) × (put|multipart) 共用的统计 / 计时 / CPU rusage
// / JSON 输出。header-only，故意小且零依赖，便于 example 加进去。
//
// 用法（最小化）：
//   bench::Runner runner({
//       .path_label = "http",
//       .mode_label = "put",
//       .threads = args.threads,
//       .count = args.count,
//       .object_size = args.object_size,
//       .warmup = args.warmup,
//   });
//   for (warmup) runner.SubmitWarmup(...);
//   runner.BeginMeasured();
//   for (count) { auto t0=clock::now(); ...do RPC...; runner.RecordLatency(t0, clock::now()); }
//   runner.End(success_count);
//   runner.PrintHuman(std::cerr);
//   runner.PrintJson(std::cout);
//
// 设计原则：
//   - Runner 不直接调任何 RPC；调用方决定怎么发起 PUT/multipart。
//   - 时间统计只统计 BeginMeasured 之后 + 调用 RecordLatency 时刻；warm-up 不计入。
//   - CPU 用 getrusage(RUSAGE_SELF)：进程级 user+sys 时间，包含所有 worker thread。
//
// JSON schema（一行）：
//   {"path","mode","threads","count","object_size","warmup",
//    "wall_s","throughput_mbps",
//    "lat_avg_ms","lat_p50_ms","lat_p95_ms","lat_p99_ms",
//    "cpu_user_s","cpu_sys_s","cpu_pct","failed"}
//   cpu_pct = (cpu_user + cpu_sys) / wall * 100   (≈ "用了多少个 CPU 核 × 100")

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <ostream>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <sys/time.h>

namespace bench {

struct RunnerOptions {
  std::string path_label;        // "http" / "rdma" / "gds"
  std::string mode_label;        // "put" / "multipart"
  std::size_t threads{0};        // 客户端线程池大小（0 = SDK 默认）
  std::size_t count{0};          // 测量轮次（不含 warmup）
  std::size_t object_size{0};    // 单次对象大小（字节）
  std::size_t warmup{0};         // warmup 笔数（不计入）
};

struct CpuTimes {
  double user_s{0.0};
  double sys_s{0.0};
};

inline CpuTimes ReadRusage() {
  rusage r{};
  ::getrusage(RUSAGE_SELF, &r);
  return {r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6,
          r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6};
}

inline double Percentile(std::vector<double>& v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const double idx = (v.size() - 1) * p;
  const std::size_t lo = static_cast<std::size_t>(idx);
  const std::size_t hi = std::min<std::size_t>(lo + 1, v.size() - 1);
  const double frac = idx - static_cast<double>(lo);
  return v[lo] * (1.0 - frac) + v[hi] * frac;
}

class Runner {
 public:
  explicit Runner(RunnerOptions opts) : opts_(std::move(opts)) {
    latencies_ms_.reserve(opts_.count);
  }

  // 正式测量开始：清零 CPU + 时间戳基准。Warm-up 阶段在此之前 / 此处之前
  // 完成；warm-up 内部时间不会被记录。
  void BeginMeasured() {
    cpu_begin_ = ReadRusage();
    wall_begin_ = std::chrono::steady_clock::now();
    measuring_ = true;
  }

  // 记录一笔 RPC 的端到端延迟（submit→complete）。
  void RecordLatency(std::chrono::steady_clock::time_point t0,
                     std::chrono::steady_clock::time_point t1) {
    if (!measuring_) return;
    const auto ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    latencies_ms_.push_back(ms);
  }

  // 调用方在所有 RPC 完成后调；failed_count 用于报错统计但不影响吞吐计算
  // （吞吐基于 latencies_ms_ 实际记录条数 × object_size）。
  void End(std::size_t failed_count = 0) {
    wall_end_ = std::chrono::steady_clock::now();
    cpu_end_  = ReadRusage();
    failed_   = failed_count;
    measuring_ = false;
  }

  // 总字节数 / wall 秒 = 吞吐（MiB/s）。
  [[nodiscard]] double ThroughputMiBps() const {
    const double ws = WallSeconds();
    if (ws <= 0) return 0.0;
    const double total =
        static_cast<double>(latencies_ms_.size()) *
        static_cast<double>(opts_.object_size);
    return total / (1024.0 * 1024.0) / ws;
  }

  [[nodiscard]] double WallSeconds() const {
    return std::chrono::duration<double>(wall_end_ - wall_begin_).count();
  }

  [[nodiscard]] double CpuUserSeconds() const {
    return cpu_end_.user_s - cpu_begin_.user_s;
  }
  [[nodiscard]] double CpuSysSeconds() const {
    return cpu_end_.sys_s - cpu_begin_.sys_s;
  }
  [[nodiscard]] double CpuPct() const {
    const double ws = WallSeconds();
    if (ws <= 0) return 0.0;
    return (CpuUserSeconds() + CpuSysSeconds()) / ws * 100.0;
  }

  void PrintHuman(std::ostream& os) {
    auto v = latencies_ms_;  // copy for Percentile
    const double p50 = Percentile(v, 0.50);
    const double p95 = Percentile(v, 0.95);
    const double p99 = Percentile(v, 0.99);
    const double avg = v.empty() ? 0.0 :
        std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    os << "==== bench " << opts_.path_label << ":" << opts_.mode_label
       << " ====" << std::endl
       << "  threads=" << opts_.threads << " count=" << opts_.count
       << " object_size=" << opts_.object_size << " warmup=" << opts_.warmup
       << " failed=" << failed_ << std::endl
       << "  wall=" << WallSeconds() << " s"
       << " throughput=" << ThroughputMiBps() << " MiB/s" << std::endl
       << "  latency_ms: avg=" << avg
       << " p50=" << p50 << " p95=" << p95 << " p99=" << p99 << std::endl
       << "  cpu: user=" << CpuUserSeconds()
       << " sys=" << CpuSysSeconds()
       << " pct=" << CpuPct() << " %" << std::endl;
  }

  void PrintJson(std::ostream& os) {
    auto v = latencies_ms_;
    const double p50 = Percentile(v, 0.50);
    const double p95 = Percentile(v, 0.95);
    const double p99 = Percentile(v, 0.99);
    const double avg = v.empty() ? 0.0 :
        std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    os << "{"
       << "\"path\":\"" << opts_.path_label << "\""
       << ",\"mode\":\"" << opts_.mode_label << "\""
       << ",\"threads\":" << opts_.threads
       << ",\"count\":" << opts_.count
       << ",\"object_size\":" << opts_.object_size
       << ",\"warmup\":" << opts_.warmup
       << ",\"failed\":" << failed_
       << ",\"wall_s\":" << WallSeconds()
       << ",\"throughput_mbps\":" << ThroughputMiBps()
       << ",\"lat_avg_ms\":" << avg
       << ",\"lat_p50_ms\":" << p50
       << ",\"lat_p95_ms\":" << p95
       << ",\"lat_p99_ms\":" << p99
       << ",\"cpu_user_s\":" << CpuUserSeconds()
       << ",\"cpu_sys_s\":" << CpuSysSeconds()
       << ",\"cpu_pct\":" << CpuPct()
       << "}" << std::endl;
  }

 private:
  RunnerOptions opts_;
  CpuTimes cpu_begin_{};
  CpuTimes cpu_end_{};
  std::chrono::steady_clock::time_point wall_begin_{};
  std::chrono::steady_clock::time_point wall_end_{};
  std::vector<double> latencies_ms_;
  std::size_t failed_{0};
  bool measuring_{false};
};

}  // namespace bench
