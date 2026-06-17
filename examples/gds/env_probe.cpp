// Minimal environment probe: exercise the CUDA runtime calls that the
// rest of the project depends on, one at a time, with explicit error
// reporting. Useful for distinguishing driver/runtime mismatches from
// application bugs.

#include <cuda_runtime.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace {

int Step(const char* label, cudaError_t e) {
  std::cout << "[" << label << "] rc=" << static_cast<int>(e)
            << " name=" << cudaGetErrorName(e)
            << " msg=" << cudaGetErrorString(e) << std::endl;
  return e == cudaSuccess ? 0 : 1;
}

}  // namespace

int main() {
  std::cout << "CUDA runtime probe" << std::endl;

  int driver_version = 0;
  if (Step("cudaDriverGetVersion",
            cudaDriverGetVersion(&driver_version)) != 0) return 1;
  std::cout << "  driver_version=" << driver_version << std::endl;

  int runtime_version = 0;
  if (Step("cudaRuntimeGetVersion",
            cudaRuntimeGetVersion(&runtime_version)) != 0) return 1;
  std::cout << "  runtime_version=" << runtime_version << std::endl;

  int device_count = 0;
  if (Step("cudaGetDeviceCount",
            cudaGetDeviceCount(&device_count)) != 0) return 1;
  std::cout << "  device_count=" << device_count << std::endl;
  if (device_count <= 0) {
    std::cerr << "no CUDA devices" << std::endl;
    return 1;
  }

  if (Step("cudaSetDevice", cudaSetDevice(0)) != 0) return 1;

  cudaDeviceProp props{};
  if (Step("cudaGetDeviceProperties",
            cudaGetDeviceProperties(&props, 0)) != 0) return 1;
  std::cout << "  device[0]: " << props.name
            << " sm=" << props.major << "." << props.minor
            << " total_mem=" << props.totalGlobalMem << std::endl;

  void* dev = nullptr;
  const std::size_t sz = 4UL * 1024 * 1024;
  if (Step("cudaMalloc(4MiB)", cudaMalloc(&dev, sz)) != 0) return 1;
  std::cout << "  ptr=" << dev << std::endl;

  if (Step("cudaMemset", cudaMemset(dev, 0xa5, sz)) != 0) return 1;
  if (Step("cudaDeviceSynchronize", cudaDeviceSynchronize()) != 0) return 1;

  // H2D + D2H round trip on a small slice to validate buffer is real.
  char host_in[64];
  char host_out[64];
  std::memset(host_in, 0x5a, sizeof(host_in));
  std::memset(host_out, 0, sizeof(host_out));
  if (Step("cudaMemcpy H2D",
            cudaMemcpy(dev, host_in, sizeof(host_in),
                       cudaMemcpyHostToDevice)) != 0) return 1;
  if (Step("cudaMemcpy D2H",
            cudaMemcpy(host_out, dev, sizeof(host_out),
                       cudaMemcpyDeviceToHost)) != 0) return 1;
  const bool same = std::memcmp(host_in, host_out, sizeof(host_in)) == 0;
  std::cout << "  h2d_d2h_roundtrip same=" << (same ? "true" : "false")
            << std::endl;

  if (Step("cudaFree", cudaFree(dev)) != 0) return 1;
  std::cout << "ALL OK" << std::endl;
  return 0;
}
