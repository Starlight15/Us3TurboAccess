// HTTP V2 并发 Ranged GET 端到端测试：
//   1. PUT 一个大对象（默认 100 MiB）
//   2. 单连接 GET 整对象 → 拿基线 wall time
//   3. 并发 GET chunks=N → 拿并发 wall time + memcmp
// 用法：http_parallel_get_example <endpoint> <bytes> <bucket> <key> [chunks=8]

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "us3_turbo_access/client/client.h"

int main(int argc, char** argv) {
  using namespace us3_turbo_access::client;
  if (argc < 5 || argc > 6) {
    std::cerr << "Usage: " << argv[0]
              << " <endpoint> <bytes> <bucket> <key> [chunks=8]" << std::endl;
    return 1;
  }
  const std::string endpoint = argv[1];
  const std::size_t bytes    = std::strtoull(argv[2], nullptr, 10);
  const std::string bucket   = argv[3];
  const std::string key      = argv[4];
  const std::size_t chunks   = argc == 6 ? std::strtoull(argv[5], nullptr, 10) : 8;

  ClientOptions options;
  options.endpoint  = endpoint;
  options.client_id = "us3-http-parallel-get-example";
  options.data_path = DataPath::kHttpTcp;
  options.async_worker_threads = chunks;
  // 把阈值调到 0 让并发 GET 一定触发；chunks 用命令行参数。
  options.http.parallel_get_threshold = 0;
  options.http.parallel_get_chunks    = chunks;
  Client client(std::move(options));
  if (auto init = client.Initialize(); !init.success()) {
    std::cerr << "Initialize failed: " << init.error().message << std::endl;
    return 1;
  }

  // 准备 payload 并 PUT。
  std::vector<std::byte> payload(bytes);
  for (std::size_t i = 0; i < bytes; ++i) {
    payload[i] = static_cast<std::byte>(i % 251U);
  }
  ConstBufferView put_buf{.data = payload.data(), .size = bytes,
                          .type = BufferType::kHostRegular};
  RequestOptions put_req;
  put_req.object = ObjectId{.bucket = bucket, .key = key};
  put_req.length = bytes;
  auto put = client.PutObject(put_req, put_buf);
  if (!put.success()) {
    std::cerr << "PUT failed: " << put.error().message << std::endl;
    return 1;
  }
  std::cout << "PUT bytes=" << bytes << " etag=" << put.value().etag << std::endl;

  // 1) 基线：单连接 GET（length 不设，HttpTransferPath 自然走 single 路径）。
  std::vector<std::byte> out_baseline(bytes);
  RequestOptions baseline_req;
  baseline_req.object = put_req.object;
  // 不设 length → GetObject 走 GetObjectSingle（不知道大小不能切）。
  MutableBufferView base_view{.data = out_baseline.data(),
                              .size = bytes,
                              .type = BufferType::kHostRegular};
  const auto t0 = std::chrono::steady_clock::now();
  auto g0 = client.GetObject(baseline_req, base_view);
  const auto t1 = std::chrono::steady_clock::now();
  if (!g0.success()) {
    std::cerr << "Baseline GET failed: " << g0.error().message << std::endl;
    return 1;
  }
  const auto baseline_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
  std::cout << "GET baseline (single): " << baseline_ms << " ms" << std::endl;

  // 2) 并发：length 给 bytes，触发 parallel get。
  std::vector<std::byte> out_parallel(bytes);
  RequestOptions par_req;
  par_req.object = put_req.object;
  par_req.length = bytes;       // 触发 parallel 路径
  MutableBufferView par_view{.data = out_parallel.data(),
                             .size = bytes,
                             .type = BufferType::kHostRegular};
  const auto t2 = std::chrono::steady_clock::now();
  auto g1 = client.GetObject(par_req, par_view);
  const auto t3 = std::chrono::steady_clock::now();
  if (!g1.success()) {
    std::cerr << "Parallel GET failed: " << g1.error().message << std::endl;
    return 1;
  }
  if (g1.value().bytes_transferred != bytes) {
    std::cerr << "Parallel GET size mismatch: got "
              << g1.value().bytes_transferred << " expected " << bytes << std::endl;
    return 1;
  }
  if (std::memcmp(out_parallel.data(), payload.data(), bytes) != 0) {
    std::cerr << "Parallel GET payload mismatch" << std::endl;
    return 1;
  }
  const auto par_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
  std::cout << "GET parallel (chunks=" << chunks << "): " << par_ms
            << " ms  same=true" << std::endl;
  if (par_ms > 0 && baseline_ms > 0) {
    const double speedup = static_cast<double>(baseline_ms) / static_cast<double>(par_ms);
    std::cout << "speedup ≈ " << speedup << "x" << std::endl;
  }

  std::cout << "OK" << std::endl;
  return 0;
}
