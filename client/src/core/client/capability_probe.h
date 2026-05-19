#pragma once

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

/**
 * @brief 探测本机 GDS / RDMA 相关能力。
 *
 * 当前只关注 GDS 路径需要的 CUDA / GPU / cuObject 可用性；
 * RDMA 字段仅为未来扩展占位。
 */
[[nodiscard]] PlatformCapabilities DetectPlatformCapabilities(const ClientOptions& options);

}  // namespace us3_turbo_access::client
