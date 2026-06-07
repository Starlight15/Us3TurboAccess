#include "client/src/transports/ucx/ucx_context.h"

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

Result<std::unique_ptr<UcxContext>> UcxContext::Create() {
  // RMA READ + AM 模式：禁 cma/shm（跨进程地址无效）
  ucp_config_t* config = nullptr;
  ucp_config_read(nullptr, nullptr, &config);
  if (config != nullptr) {
    ucp_config_modify(config, "TLS", "rc,ud,tcp");
  }

  ucp_params_t params{};
  params.field_mask = UCP_PARAM_FIELD_FEATURES;
  params.features   = UCP_FEATURE_RMA | UCP_FEATURE_AM;

  ucp_context_h ctx = nullptr;
  ucs_status_t st = ucp_init(&params, config, &ctx);
  if (config != nullptr) ucp_config_release(config);

  if (st != UCS_OK) {
    return Result<std::unique_ptr<UcxContext>>::Failure(
        MakeRegistrationFailure(ucs_status_string(st)));
  }

  auto obj = std::unique_ptr<UcxContext>(new UcxContext());
  obj->ctx_ = ctx;
  return Result<std::unique_ptr<UcxContext>>::Success(std::move(obj));
}

UcxContext::~UcxContext() {
  if (ctx_ != nullptr) {
    ucp_cleanup(ctx_);
    ctx_ = nullptr;
  }
}

}  // namespace us3_turbo_access::client
