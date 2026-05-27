#pragma once

// RDMA 控制面与数据面之间通过 librdmacm 的 ACCEPT private_data 传递的
// 服务端凭据。布局必须在 server/client 编译单元里二进制一致，因此放在
// 共享 include 路径下，由 gateway 与 client 同时 include。
//
// 版本兼容：未来字段扩展时通过 version 推进；旧 client 看到不识别的版本
// 时应当拒绝连接，避免误解析。

#include <cstdint>

namespace us3_turbo_access::common {

inline constexpr std::uint32_t kRdmaCredentialsMagic   = 0x52543355u;  // 'U3TR'
inline constexpr std::uint32_t kRdmaCredentialsVersion = 1;

/**
 * server 在 rdma_accept 阶段写入 private_data，client 在 ESTABLISHED 事件
 * 上读取。conn_token 标识 server 端 RdmaConnectionRegistry 里的连接条目。
 */
struct RdmaConnectCredentials {
  std::uint32_t magic;
  std::uint32_t version;
  std::uint64_t conn_token;
};
static_assert(sizeof(RdmaConnectCredentials) == 16,
              "wire layout must stay 16 bytes for forward compat");

}  // namespace us3_turbo_access::common
