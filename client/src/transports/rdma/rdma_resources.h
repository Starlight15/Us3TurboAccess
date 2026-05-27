#pragma once

#include <memory>
#include <string>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

struct ibv_context;

namespace us3_turbo_access::client {

/**
 * 客户端 RDMA 设备探活：仅打开 device 并暴露 device_name，用于
 * ClientCore::Initialize 时设置 RdmaTransferPath::SetAvailable。
 *
 * 实际 PD/CQ/QP/MR 在每次 PutObject 现场基于 cm_id->verbs 建（client
 * 端的 RdmaCmConnection 负责），所以这里不再缓存 PD/CQ/comp_channel。
 */
class RdmaResources {
 public:
  [[nodiscard]] static Result<std::unique_ptr<RdmaResources>>
    Open(const RdmaClientOptions& opts);

  ~RdmaResources();

  RdmaResources(const RdmaResources&) = delete;
  RdmaResources& operator=(const RdmaResources&) = delete;

  [[nodiscard]] const std::string& device_name() const noexcept { return device_name_; }

 private:
  RdmaResources() = default;

  ibv_context*      ctx_{nullptr};
  std::string       device_name_;
};

}  // namespace us3_turbo_access::client
