#include "data_path/rdma/rdma_resources.h"

#include <arpa/inet.h>
#include <infiniband/verbs.h>

#include <cstdio>
#include <cstring>

#include "common/error.h"

namespace us3_turbo_access::gateway::data_path::rdma {

namespace {

[[nodiscard]] Error MakeRdmaError(std::string message) {
  Error err;
  err.code = ErrorCode::kRdmaUnavailable;
  err.message = std::move(message);
  err.retryable = false;
  return err;
}

[[nodiscard]] ibv_context* OpenDeviceByName(const std::string& name) {
  int num = 0;
  ibv_device** list = ibv_get_device_list(&num);
  if (list == nullptr || num == 0) {
    if (list) ibv_free_device_list(list);
    return nullptr;
  }
  ibv_context* picked = nullptr;
  for (int i = 0; i < num; ++i) {
    if (!name.empty() &&
        std::strcmp(ibv_get_device_name(list[i]), name.c_str()) != 0) {
      continue;
    }
    picked = ibv_open_device(list[i]);
    if (picked != nullptr) break;
  }
  ibv_free_device_list(list);
  return picked;
}

[[nodiscard]] std::string FormatGid(const ibv_gid& gid) {
  char buf[INET6_ADDRSTRLEN] = {0};
  std::snprintf(buf, sizeof(buf),
                "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                gid.raw[0], gid.raw[1], gid.raw[2], gid.raw[3],
                gid.raw[4], gid.raw[5], gid.raw[6], gid.raw[7],
                gid.raw[8], gid.raw[9], gid.raw[10], gid.raw[11],
                gid.raw[12], gid.raw[13], gid.raw[14], gid.raw[15]);
  return std::string(buf);
}

}  // namespace

// 仅做：选 device、读 GID。PD/CQ/comp_channel 全部留给 cm_id 现场建。
Result<std::unique_ptr<RdmaResources>> RdmaResources::Open(
    const RdmaOptions& opts) {
  auto res = std::unique_ptr<RdmaResources>(new RdmaResources());
  res->ib_port_ = opts.ib_port;

  res->ctx_ = OpenDeviceByName(opts.device_name);
  if (res->ctx_ == nullptr) {
    return Result<std::unique_ptr<RdmaResources>>::Failure(MakeRdmaError(
        opts.device_name.empty()
            ? "no RDMA device found"
            : "RDMA device not found: " + opts.device_name));
  }
  res->device_name_ = ibv_get_device_name(res->ctx_->device);

  ibv_gid gid{};
  if (ibv_query_gid(res->ctx_, opts.ib_port, opts.gid_index, &gid) != 0) {
    return Result<std::unique_ptr<RdmaResources>>::Failure(
        MakeRdmaError("ibv_query_gid failed"));
  }
  res->gid_string_ = FormatGid(gid);

  return Result<std::unique_ptr<RdmaResources>>::Success(std::move(res));
}

RdmaResources::~RdmaResources() {
  if (ctx_ != nullptr) (void)ibv_close_device(ctx_);
}

}  // namespace us3_turbo_access::gateway::data_path::rdma
