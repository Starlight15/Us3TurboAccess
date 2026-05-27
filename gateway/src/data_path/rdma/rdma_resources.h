#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "us3_turbo_access/gateway/options.h"
#include "us3_turbo_access/gateway/result.h"

struct ibv_context;

namespace us3_turbo_access::gateway::data_path::rdma {

/**
 * 服务端 RDMA 探活信息：device context + 字符串化的 GID。
 *
 * PD/CQ/QP 在 R2 之后全部按 cm_id->verbs 现场建（必须与 librdmacm 路由选中的
 * verbs context 同源），所以本对象不再缓存共享 PD/CQ。仅保留 ibv_context
 * 的 owner 引用，以便：
 *   1) 启动期 query GID（写进日志/响应）；
 *   2) 进程退出时干净 close device。
 *
 * 调用方语义：从 Open() 拿 unique_ptr，用 device_name() / gid_string() 即可，
 * 不要再访问 ctx_。
 */
class RdmaResources {
 public:
  [[nodiscard]] static Result<std::unique_ptr<RdmaResources>>
    Open(const RdmaOptions& opts);

  ~RdmaResources();

  RdmaResources(const RdmaResources&) = delete;
  RdmaResources& operator=(const RdmaResources&) = delete;

  [[nodiscard]] const std::string& device_name() const noexcept { return device_name_; }
  [[nodiscard]] const std::string& gid_string()  const noexcept { return gid_string_; }
  [[nodiscard]] std::uint8_t       ib_port()     const noexcept { return ib_port_; }

 private:
  RdmaResources() = default;

  ibv_context*      ctx_{nullptr};
  std::string       device_name_;
  std::string       gid_string_;
  std::uint8_t      ib_port_{1};
};

}  // namespace us3_turbo_access::gateway::data_path::rdma
