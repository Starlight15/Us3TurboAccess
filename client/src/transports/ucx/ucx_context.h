#pragma once

#include <memory>

#include <ucp/api/ucp.h>

#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

/**
 * 进程级 UCP context 的 RAII 封装。
 *
 * 一个进程通常只需一个 UcxContext；调用方通过 Create() 工厂构造，
 * 析构时自动调用 ucp_cleanup。
 *
 * features 固定启用 RMA + AM（PUT 单边写 + active message 用于 rkey 交换）。
 */
class UcxContext {
 public:
  [[nodiscard]] static Result<std::unique_ptr<UcxContext>> Create();

  ~UcxContext();

  UcxContext(const UcxContext&) = delete;
  UcxContext& operator=(const UcxContext&) = delete;

  [[nodiscard]] ucp_context_h handle() const noexcept { return ctx_; }

 private:
  UcxContext() = default;

  ucp_context_h ctx_{nullptr};
};

}  // namespace us3_turbo_access::client
