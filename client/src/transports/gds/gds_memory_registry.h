#pragma once

#include <string>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

struct RegisteredBuffer {
  std::string memory_descriptor;
};

/**
 * @brief GDS GPU buffer 注册器。
 *
 * 当前网关只需要客户端把 buffer descriptor 作为 session 协商参数传入，
 * 因此这里封装 descriptor 的生成与基础参数校验。
 */
class GdsMemoryRegistry {
 public:
  [[nodiscard]] Result<RegisteredBuffer> Register(OperationType operation,
                                                  MutableBufferView buffer) const;
  [[nodiscard]] Result<RegisteredBuffer> Register(OperationType operation,
                                                  ConstBufferView buffer) const;
};

}  // namespace us3_turbo_access::client
