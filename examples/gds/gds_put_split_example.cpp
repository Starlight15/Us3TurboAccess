// gds_put_split_example.cpp
// 验证 proxy/backend 拆分模式下的 GDS 单对象 PUT 链路：
//   Client → Proxy(OpenSession) → Client → Backend(GdsPut+RDMA)
//   → Backend → Proxy(ReportGdsPut)
//
// Usage:
//   gds_put_split_example <proxy_endpoint> <backend_endpoint> <bytes> <bucket> <key>
//
// 环境变量覆盖（可选）：
//   GDS_PROXY_ENDPOINT   覆盖 proxy 地址
//   GDS_BACKEND_ENDPOINT 覆盖 backend 地址

#include <cuda_runtime.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "us3_turbo_access/client/client.h"

namespace {

[[nodiscard]] std::string EnvOr(const char* name, std::string fallback) {
  const char* v = std::getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

[[nodiscard]] bool CudaCheck(cudaError_t s, const char* op) {
  if (s == cudaSuccess) return true;
  std::cerr << op << ": " << cudaGetErrorString(s) << "\n";
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo_access::client;

  if (argc != 6) {
    std::cerr << "Usage: " << argv[0]
              << " <proxy_endpoint> <backend_endpoint> <bytes> <bucket> <key>\n";
    return 1;
  }

  const std::string proxy_ep   = EnvOr("GDS_PROXY_ENDPOINT",   argv[1]);
  const std::string backend_ep = EnvOr("GDS_BACKEND_ENDPOINT", argv[2]);
  const std::size_t bytes      = static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10));
  const std::string bucket     = argv[4];
  const std::string key        = argv[5];

  std::cerr << "[cfg] proxy=" << proxy_ep
            << " backend=" << backend_ep
            << " bytes=" << bytes
            << " bucket=" << bucket
            << " key=" << key << "\n";

  // ---- 分配并初始化 GPU 内存 ----
  void* dev = nullptr;
  if (!CudaCheck(cudaMalloc(&dev, bytes), "cudaMalloc")) return 1;
  // 填充可复现的测试数据（index % 251）
  std::vector<std::byte> host(bytes);
  for (std::size_t i = 0; i < bytes; ++i)
    host[i] = static_cast<std::byte>(i % 251U);
  if (!CudaCheck(cudaMemcpy(dev, host.data(), bytes, cudaMemcpyHostToDevice),
                 "cudaMemcpy")) {
    cudaFree(dev);
    return 1;
  }

  // ---- 初始化 Client（proxy 控制面 + backend 数据面分离）----
  ClientOptions opts;
  opts.endpoint          = proxy_ep;    // OpenSession → proxy
  opts.gds_data_endpoint = backend_ep;  // GdsPut     → backend
  opts.client_id         = "gds-put-split-example";
  opts.data_path         = DataPath::kGdsCuObject;

  Client client(std::move(opts));
  if (auto r = client.Initialize(); !r.success()) {
    std::cerr << "Initialize: " << r.error().message << "\n";
    cudaFree(dev);
    return 1;
  }

  // ---- 注册显存（cuObj RDMA 注册） ----
  if (auto r = client.RegisterDeviceBuffer(dev, bytes); !r.success()) {
    std::cerr << "RegisterDeviceBuffer: " << r.error().message << "\n";
    cudaFree(dev);
    return 1;
  }

  // ---- PUT（链路：OpenSession→proxy + GdsPut→backend + backend→proxy通知）----
  RequestOptions req;
  req.object  = ObjectId{.bucket = bucket, .key = key};
  req.length  = bytes;  // proxy 校验 expected_size > 0

  auto put = client.PutObject(
      req, ConstBufferView{.data = dev, .size = bytes, .type = BufferType::kCudaDevice});

  // ---- 反注册显存后释放 GPU 内存 ----
  (void)client.UnregisterDeviceBuffer(dev);
  cudaFree(dev);

  if (!put.success()) {
    std::cerr << "PutObject FAILED: " << put.error().message << "\n";
    return 1;
  }

  std::cout << "OK path=" << ToString(put.value().selected_path)
            << " bytes=" << put.value().bytes_transferred
            << " etag=" << put.value().etag
            << " session=" << put.value().session_id << "\n";
  return 0;
}
