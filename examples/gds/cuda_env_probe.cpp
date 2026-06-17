/*
 * CUDA environment probe — 快速验证 CUDA 运行时环境完整性
 *
 * 目的：
 *   单步验证 CUDA 运行时 API 调用链路，明确区分驱动/运行时版本不匹配、
 *   设备不可用、内存分配失败等常见环境问题。用于新环境部署、驱动升级后
 *   的第一道检查，避免将环境问题误判为业务代码 bug。
 *
 * 执行原理（按依赖顺序逐步验证）：
 *   1. cudaDriverGetVersion / cudaRuntimeGetVersion — 版本兼容性
 *   2. cudaGetDeviceCount / cudaSetDevice(0)       — 设备可见性
 *   3. cudaGetDeviceProperties                     — SM 版本、显存大小
 *   4. cudaMalloc / cudaMemset                     — GPU 内存分配与初始化
 *   5. cudaMemcpy H2D + D2H round trip             — 主机↔设备传输正确性
 *   6. cudaFree                                    — 资源清理
 *
 * 常见失败场景：
 *   - cudaDriverGetVersion 失败 → 驱动未加载或版本过旧（需 ≥ 11.x）
 *   - device_count=0           → 无 GPU 或被 MIG/exclusive 模式独占
 *   - cudaMalloc 失败          → 显存不足；先运行 nvidia-smi 检查占用
 *   - H2D/D2H round trip 不一致 → PCIe 通道错误或 ECC 内存校正失败
 *
 * 用法：
 *   ./us3_turbo_access_cuda_env_probe
 *   所有步骤通过时输出 "ALL OK"，任一步骤失败立即退出并打印出错信息
 */

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
