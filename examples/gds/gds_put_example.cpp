#include <cuda_runtime.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "us3_turbo_access/client/client.h"

namespace us3_turbo_access::client {
namespace {

[[nodiscard]] bool CheckCuda(cudaError_t status, const char* action) {
  if (status == cudaSuccess) {
    return true;
  }
  std::cerr << action << " failed: " << cudaGetErrorString(status) << std::endl;
  return false;
}

}  // namespace
}  // namespace us3_turbo_access::client

int main(int argc, char** argv) {
  using namespace us3_turbo_access::client;

  if (argc != 5) {
    std::cerr << "Usage: " << argv[0]
              << " <endpoint> <bytes> <bucket> <object_key>" << std::endl;
    return 1;
  }

  const std::string endpoint = argv[1];
  const std::size_t bytes = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
  const std::string bucket = argv[3];
  const std::string object_key = argv[4];

  std::vector<std::byte> host_upload(bytes);
  std::vector<std::byte> host_download(bytes);
  for (std::size_t index = 0; index < host_upload.size(); ++index) {
    host_upload[index] = static_cast<std::byte>(index % 251U);
  }

  void* device_upload = nullptr;
  void* device_download = nullptr;
  if (!CheckCuda(cudaMalloc(&device_upload, bytes), "cudaMalloc upload") ||
      !CheckCuda(cudaMalloc(&device_download, bytes), "cudaMalloc download") ||
      !CheckCuda(cudaMemcpy(device_upload, host_upload.data(), bytes, cudaMemcpyHostToDevice),
                 "cudaMemcpy host->device") ||
      !CheckCuda(cudaMemset(device_download, 0, bytes), "cudaMemset download")) {
    if (device_upload != nullptr) {
      cudaFree(device_upload);
    }
    if (device_download != nullptr) {
      cudaFree(device_download);
    }
    return 1;
  }

  ClientOptions options;
  options.endpoint = endpoint;
  options.client_id = "us3-gds-example";
  options.data_path = DataPath::kGdsCuObject;

  Client client(std::move(options));
  auto init_result = client.Initialize();
  if (!init_result.success()) {
    std::cerr << "Initialize client failed: " << init_result.error().message << std::endl;
    cudaFree(device_upload);
    cudaFree(device_download);
    return 1;
  }

  // cudaMalloc 后立即注册到 cuObj descriptor 表（cudaFree 之前必须反注册）。
  if (auto r = client.RegisterDeviceBuffer(device_upload, bytes); !r.success()) {
    std::cerr << "RegisterDeviceBuffer(upload) failed: " << r.error().message << std::endl;
    cudaFree(device_upload); cudaFree(device_download); return 1;
  }
  if (auto r = client.RegisterDeviceBuffer(device_download, bytes); !r.success()) {
    std::cerr << "RegisterDeviceBuffer(download) failed: " << r.error().message << std::endl;
    client.UnregisterDeviceBuffer(device_upload);
    cudaFree(device_upload); cudaFree(device_download); return 1;
  }

  RequestOptions request;
  request.object = ObjectId{.bucket = bucket, .key = object_key};

  auto put_result = client.PutObject(
      request, ConstBufferView{.data = device_upload, .size = bytes, .type = BufferType::kCudaDevice});
  if (!put_result.success()) {
    std::cerr << "PutObject failed: " << put_result.error().message << std::endl;
    cudaFree(device_upload);
    cudaFree(device_download);
    return 1;
  }
  std::cout << "PUT path=" << ToString(put_result.value().selected_path)
            << " bytes=" << put_result.value().bytes_transferred << std::endl;

  auto head_result = client.HeadObject(request.object);
  if (!head_result.success()) {
    std::cerr << "HeadObject failed: " << head_result.error().message << std::endl;
    cudaFree(device_upload);
    cudaFree(device_download);
    return 1;
  }
  std::cout << "HEAD size=" << head_result.value().content_length << std::endl;

  auto get_result = client.GetObject(
      request, MutableBufferView{.data = device_download, .size = bytes, .type = BufferType::kCudaDevice});
  if (!get_result.success()) {
    std::cerr << "GetObject failed: " << get_result.error().message << std::endl;
    cudaFree(device_upload);
    cudaFree(device_download);
    return 1;
  }
  std::cout << "GET path=" << ToString(get_result.value().selected_path)
            << " bytes=" << get_result.value().bytes_transferred << std::endl;

  if (!CheckCuda(cudaMemcpy(host_download.data(), device_download, bytes, cudaMemcpyDeviceToHost),
                 "cudaMemcpy device->host")) {
    cudaFree(device_upload);
    cudaFree(device_download);
    return 1;
  }

  const bool same = std::memcmp(host_upload.data(), host_download.data(), bytes) == 0;
  std::cout << "same=" << (same ? "true" : "false") << std::endl;

  // cudaFree 前先反注册
  (void)client.UnregisterDeviceBuffer(device_upload);
  (void)client.UnregisterDeviceBuffer(device_download);
  cudaFree(device_upload);
  cudaFree(device_download);
  return same ? 0 : 1;
}
