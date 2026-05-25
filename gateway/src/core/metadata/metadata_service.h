#pragma once

#include <cstddef>
#include <string_view>

#include "backend/backend.h"
#include "us3_turbo_access/gateway/result.h"
#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway::core {

/**
 * @brief 元数据 / 预占接口，所有 data path 共享。
 *
 * 仅做语义转发，让 backend 操作有一个稳定的、跟 data path 解耦的入口。
 * Head 用于 HEAD 与会话开启前的对象探测；Reserve 用于 PUT session 预占空间。
 */
class MetadataService {
 public:
  explicit MetadataService(backend::IBackend& backend) : backend_(backend) {}

  [[nodiscard]] Result<ObjectMetadata> Head(std::string_view bucket,
                                             std::string_view key) {
    return backend_.Head(bucket, key);
  }

  [[nodiscard]] Result<ObjectMetadata> Reserve(std::string_view bucket,
                                                std::string_view key,
                                                std::size_t total_size) {
    return backend_.Reserve(bucket, key, total_size);
  }

 private:
  backend::IBackend& backend_;
};

}  // namespace us3_turbo_access::gateway::core
