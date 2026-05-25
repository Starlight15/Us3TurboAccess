#pragma once

#include <string>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

/**
 * @brief Result of a successful Register() call. Kept as a struct so the
 *        existing code style (.success() / .value().xxx access) doesn't
 *        need to change; no public fields are needed now that gateway no
 *        longer consumes the memory descriptor.
 */
struct RegisteredBuffer {};

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
