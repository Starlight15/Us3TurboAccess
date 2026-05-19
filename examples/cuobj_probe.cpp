#include <cuda_runtime.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <string>
#include <vector>

#include <cufile.h>
#include <cuobjclient.h>

namespace {

constexpr const char* kCuObjLibraryCandidates[] = {
    "/usr/local/cuda/targets/x86_64-linux/lib/libcuobjclient.so.1",
    "/usr/local/cuda-13.2/targets/x86_64-linux/lib/libcuobjclient.so.1",
    "libcuobjclient.so.1",
    "libcuobjclient.so",
};

struct CuObjApi {
  using Constructor = void (*)(void*, CUObjOps_t&, cuObjProto_t);
  using Destructor = void (*)(void*);
  using IsConnected = bool (*)(void*);
  using GetDescriptor = cuObjErr_t (*)(void*, void*, size_t);
  using PutDescriptor = cuObjErr_t (*)(void*, void*);
  using GetCtx = void* (*)(const void*);
  using GetObject = ssize_t (*)(void*, void*, void*, size_t, loff_t, loff_t);
  using PutObject = ssize_t (*)(void*, void*, void*, size_t, loff_t, loff_t);

  Constructor constructor{nullptr};
  Destructor destructor{nullptr};
  IsConnected is_connected{nullptr};
  GetDescriptor get_descriptor{nullptr};
  PutDescriptor put_descriptor{nullptr};
  GetCtx get_ctx{nullptr};
  GetObject cuobj_get{nullptr};
  PutObject cuobj_put{nullptr};
};

struct CallbackState {
  std::atomic<int> get_calls{0};
  std::atomic<int> put_calls{0};
};

template <typename Symbol>
Symbol ResolveSymbol(void* handle, const char* name) {
  dlerror();
  void* symbol = dlsym(handle, name);
  const char* error = dlerror();
  if (error != nullptr || symbol == nullptr) {
    std::cerr << "resolve failed: " << name << " error=" << (error == nullptr ? "null" : error)
              << std::endl;
    return nullptr;
  }
  return reinterpret_cast<Symbol>(symbol);
}

bool CheckCuda(cudaError_t status, const char* action) {
  if (status == cudaSuccess) {
    return true;
  }
  std::cerr << action << " failed: " << cudaGetErrorString(status) << std::endl;
  return false;
}

std::string RdmaTokenFromInfo(const cufileRDMAInfo_t* rdma_info) {
  if (rdma_info == nullptr || rdma_info->desc_str == nullptr) {
    return {};
  }
  std::size_t len = rdma_info->desc_len > 0 ? static_cast<std::size_t>(rdma_info->desc_len)
                                            : std::strlen(rdma_info->desc_str);
  while (len > 0 && rdma_info->desc_str[len - 1] == '\0') {
    --len;
  }
  return std::string(rdma_info->desc_str, rdma_info->desc_str + len);
}

ssize_t ProbeGet(const void* handle, char* ptr, size_t size, loff_t offset,
                 const cufileRDMAInfo_t* rdma_info) {
  auto* state = const_cast<CallbackState*>(static_cast<const CallbackState*>(handle));
  if (state != nullptr) {
    state->get_calls.fetch_add(1, std::memory_order_relaxed);
  }
  std::cout << "[probe] get callback size=" << size << " offset=" << offset;
  const std::string token = RdmaTokenFromInfo(rdma_info);
  if (!token.empty()) {
    std::cout << " rdma_token_size=" << token.size();
  }
  std::cout << std::endl;
  static_cast<void>(ptr);
  return -1;
}

ssize_t ProbePut(const void* handle, const char* ptr, size_t size, loff_t offset,
                 const cufileRDMAInfo_t* rdma_info) {
  auto* state = const_cast<CallbackState*>(static_cast<const CallbackState*>(handle));
  if (state != nullptr) {
    state->put_calls.fetch_add(1, std::memory_order_relaxed);
  }
  std::cout << "[probe] put callback size=" << size << " offset=" << offset;
  const std::string token = RdmaTokenFromInfo(rdma_info);
  if (!token.empty()) {
    std::cout << " rdma_token_size=" << token.size();
  }
  std::cout << std::endl;
  static_cast<void>(ptr);
  return -1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t bytes = argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 4096U;

  void* handle = nullptr;
  const char* loaded_path = nullptr;
  for (const char* candidate : kCuObjLibraryCandidates) {
    std::cout << "[probe] dlopen candidate=" << candidate << std::endl;
    handle = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
    if (handle != nullptr) {
      loaded_path = candidate;
      break;
    }
  }
  if (handle == nullptr) {
    std::cerr << "[probe] failed to load libcuobjclient" << std::endl;
    return 1;
  }
  std::cout << "[probe] loaded=" << loaded_path << std::endl;

  CuObjApi api;
  api.constructor = ResolveSymbol<CuObjApi::Constructor>(handle, "_ZN11cuObjClientC1ER10CUObjIOOps15cuObjProto_enum");
  api.destructor = ResolveSymbol<CuObjApi::Destructor>(handle, "_ZN11cuObjClientD1Ev");
  api.is_connected = ResolveSymbol<CuObjApi::IsConnected>(handle, "_ZN11cuObjClient11isConnectedEv");
  api.get_descriptor = ResolveSymbol<CuObjApi::GetDescriptor>(handle, "_ZN11cuObjClient21cuMemObjGetDescriptorEPvm");
  api.put_descriptor = ResolveSymbol<CuObjApi::PutDescriptor>(handle, "_ZN11cuObjClient21cuMemObjPutDescriptorEPv");
  api.get_ctx = ResolveSymbol<CuObjApi::GetCtx>(handle, "_ZN11cuObjClient6getCtxEPKv");
  api.cuobj_get = ResolveSymbol<CuObjApi::GetObject>(handle, "_ZN11cuObjClient8cuObjGetEPvS0_mll");
  api.cuobj_put = ResolveSymbol<CuObjApi::PutObject>(handle, "_ZN11cuObjClient8cuObjPutEPvS0_mll");
  if (api.constructor == nullptr || api.destructor == nullptr || api.is_connected == nullptr ||
      api.get_descriptor == nullptr || api.put_descriptor == nullptr || api.get_ctx == nullptr ||
      api.cuobj_get == nullptr || api.cuobj_put == nullptr) {
    dlclose(handle);
    return 1;
  }
  std::cout << "[probe] symbols resolved" << std::endl;

  void* device_buffer = nullptr;
  if (!CheckCuda(cudaMalloc(&device_buffer, bytes), "cudaMalloc")) {
    dlclose(handle);
    return 1;
  }
  if (!CheckCuda(cudaMemset(device_buffer, 0, bytes), "cudaMemset")) {
    cudaFree(device_buffer);
    dlclose(handle);
    return 1;
  }
  std::cout << "[probe] device buffer ready bytes=" << bytes << std::endl;

  CUObjOps_t ops{};
  ops.get = &ProbeGet;
  ops.put = &ProbePut;

  alignas(alignof(cuObjClient)) std::byte storage[4096];
  std::memset(storage, 0, sizeof(storage));
  CallbackState state;

  std::cout << "[probe] constructing client" << std::endl;
  api.constructor(storage, ops, CUOBJ_PROTO_RDMA_DC_V1);
  std::cout << "[probe] client constructed" << std::endl;

  const bool connected = api.is_connected(storage);
  std::cout << "[probe] isConnected=" << (connected ? "true" : "false") << std::endl;

  std::cout << "[probe] registering descriptor" << std::endl;
  const cuObjErr_t descriptor_result = api.get_descriptor(storage, device_buffer, bytes);
  std::cout << "[probe] descriptor result=" << static_cast<int>(descriptor_result) << std::endl;

  if (descriptor_result == CU_OBJ_SUCCESS) {
    std::cout << "[probe] calling cuObjPut" << std::endl;
    const ssize_t put_result = api.cuobj_put(storage, &state, device_buffer, bytes, 0, 0);
    std::cout << "[probe] cuObjPut result=" << put_result
              << " callback_count=" << state.put_calls.load(std::memory_order_relaxed) << std::endl;

    std::cout << "[probe] calling cuObjGet" << std::endl;
    const ssize_t get_result = api.cuobj_get(storage, &state, device_buffer, bytes, 0, 0);
    std::cout << "[probe] cuObjGet result=" << get_result
              << " callback_count=" << state.get_calls.load(std::memory_order_relaxed) << std::endl;

    const cuObjErr_t put_descriptor_result = api.put_descriptor(storage, device_buffer);
    std::cout << "[probe] put descriptor result=" << static_cast<int>(put_descriptor_result) << std::endl;
  }

  std::cout << "[probe] destroying client" << std::endl;
  api.destructor(storage);
  std::cout << "[probe] client destroyed" << std::endl;

  cudaFree(device_buffer);
  dlclose(handle);
  return 0;
}
