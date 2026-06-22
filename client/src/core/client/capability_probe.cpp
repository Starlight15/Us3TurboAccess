#include "client/src/core/client/capability_probe.h"

#include <filesystem>

#if defined(__linux__)
#include <dlfcn.h>
#endif

namespace us3_turbo_access::client {
namespace {

[[nodiscard]] bool LibraryAvailable(const char* name) {
#if defined(__linux__)
  void* handle = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
  if (handle == nullptr) {
    return false;
  }
  dlclose(handle);
  return true;
#else
  static_cast<void>(name);
  return false;
#endif
}

}  // namespace

PlatformCapabilities DetectPlatformCapabilities(const ClientOptions& options) {
  PlatformCapabilities caps;
  caps.cuda_runtime_available = LibraryAvailable("libcuda.so.1") ||
                                LibraryAvailable("libcudart.so");
  caps.gpu_available = std::filesystem::exists("/dev/nvidia0");
  caps.cuobject_available =
      options.data_flow == DataFlow::GPUDirect && caps.cuda_runtime_available &&
      LibraryAvailable("libcufile.so");
  return caps;
}

}  // namespace us3_turbo_access::client
