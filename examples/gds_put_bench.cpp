#include <cuda_runtime.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "us3_turbo_access/client/client.h"

namespace {

[[nodiscard]] bool CheckCuda(cudaError_t status, const char* action) {
  if (status == cudaSuccess) {
    return true;
  }
  std::cerr << action << " failed: " << cudaGetErrorString(status) << std::endl;
  return false;
}

[[nodiscard]] double Throughput(std::size_t bytes, std::chrono::nanoseconds dt) {
  if (dt.count() <= 0) {
    return 0.0;
  }
  const double seconds = static_cast<double>(dt.count()) / 1e9;
  return (static_cast<double>(bytes) / seconds) / (1024.0 * 1024.0 * 1024.0);  // GiB/s
}

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo_access::client;

  if (argc < 4 || argc > 6) {
    std::cerr << "Usage: " << argv[0]
              << " <endpoint> <bytes> <bucket> [object_key] [iterations]"
              << std::endl;
    return 1;
  }

  const std::string endpoint = argv[1];
  const std::size_t bytes = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
  const std::string bucket = argv[3];
  const std::string object_key = argc >= 5 ? argv[4] : "claude/put-bench";
  const int iterations = argc >= 6 ? std::atoi(argv[5]) : 1;
  if (bytes == 0 || iterations <= 0) {
    std::cerr << "bytes and iterations must be positive" << std::endl;
    return 1;
  }

  std::cout << "preparing " << bytes << " bytes of GPU buffer..." << std::endl;
  void* device_buffer = nullptr;
  if (!CheckCuda(cudaMalloc(&device_buffer, bytes), "cudaMalloc")) {
    return 1;
  }
  if (!CheckCuda(cudaMemset(device_buffer, 0xAB, bytes), "cudaMemset")) {
    cudaFree(device_buffer);
    return 1;
  }
  cudaDeviceSynchronize();

  ClientOptions options;
  options.endpoint = endpoint;
  options.client_id = "us3-gds-put-bench";
  options.data_path = DataPath::kGdsCuObject;

  Client client(std::move(options));
  auto init_result = client.Initialize();
  if (!init_result.success()) {
    std::cerr << "Initialize client failed: " << init_result.error().message << std::endl;
    cudaFree(device_buffer);
    return 1;
  }

  RequestOptions request;
  request.object = ObjectId{.bucket = bucket, .key = object_key};
  const ConstBufferView buffer{.data = device_buffer,
                                .size = bytes,
                                .type = BufferType::kCudaDevice};

  using clock = std::chrono::steady_clock;
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "warming up..." << std::endl;
  {
    auto warm = client.PutObject(request, buffer);
    if (!warm.success()) {
      std::cerr << "warmup PutObject failed: " << warm.error().message << std::endl;
      cudaFree(device_buffer);
      return 1;
    }
  }

  std::chrono::nanoseconds total{};
  std::chrono::nanoseconds best{std::chrono::nanoseconds::max()};
  std::chrono::nanoseconds worst{};

  for (int i = 0; i < iterations; ++i) {
    auto t0 = clock::now();
    auto result = client.PutObject(request, buffer);
    auto dt = clock::now() - t0;
    if (!result.success()) {
      std::cerr << "PutObject iter=" << i << " failed: " << result.error().message << std::endl;
      cudaFree(device_buffer);
      return 1;
    }
    total += dt;
    best = std::min(best, dt);
    worst = std::max(worst, dt);
    std::cout << "iter=" << i
              << " bytes=" << result.value().bytes_transferred
              << " elapsed_ms=" << std::chrono::duration<double, std::milli>(dt).count()
              << " throughput=" << Throughput(bytes, dt) << " GiB/s"
              << std::endl;
  }

  const auto avg = total / iterations;
  std::cout << "---------- summary ----------" << std::endl;
  std::cout << "bytes_per_put = " << bytes << std::endl;
  std::cout << "iterations    = " << iterations << std::endl;
  std::cout << "avg_ms        = " << std::chrono::duration<double, std::milli>(avg).count() << std::endl;
  std::cout << "best_ms       = " << std::chrono::duration<double, std::milli>(best).count() << std::endl;
  std::cout << "worst_ms      = " << std::chrono::duration<double, std::milli>(worst).count() << std::endl;
  std::cout << "avg_throughput= " << Throughput(bytes, avg) << " GiB/s" << std::endl;
  std::cout << "peak_throughput=" << Throughput(bytes, best) << " GiB/s" << std::endl;

  cudaFree(device_buffer);
  return 0;
}
