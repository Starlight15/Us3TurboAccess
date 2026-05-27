#include "client/src/transports/rdma/rdma_resources.h"

#include <infiniband/verbs.h>

#include <cstring>

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

namespace {

[[nodiscard]] ibv_context* OpenDeviceByName(const std::string& name) {
  int num = 0;
  ibv_device** list = ibv_get_device_list(&num);
  if (list == nullptr || num == 0) {
    if (list) ibv_free_device_list(list);
    return nullptr;
  }
  ibv_context* picked = nullptr;
  for (int i = 0; i < num; ++i) {
    if (!name.empty() && std::strcmp(ibv_get_device_name(list[i]), name.c_str()) != 0) {
      continue;
    }
    picked = ibv_open_device(list[i]);
    if (picked != nullptr) break;
  }
  ibv_free_device_list(list);
  return picked;
}

}  // namespace

Result<std::unique_ptr<RdmaResources>> RdmaResources::Open(
    const RdmaClientOptions& opts) {
  auto res = std::unique_ptr<RdmaResources>(new RdmaResources());

  res->ctx_ = OpenDeviceByName(opts.device_name);
  if (res->ctx_ == nullptr) {
    return Result<std::unique_ptr<RdmaResources>>::Failure(MakeUnsupportedPath(
        DataPath::kNativeRdma,
        opts.device_name.empty()
            ? "未找到任何可用 RDMA 设备"
            : "未找到指定 RDMA 设备: " + opts.device_name));
  }
  res->device_name_ = ibv_get_device_name(res->ctx_->device);

  return Result<std::unique_ptr<RdmaResources>>::Success(std::move(res));
}

RdmaResources::~RdmaResources() {
  if (ctx_ != nullptr) (void)ibv_close_device(ctx_);
}

}  // namespace us3_turbo_access::client
