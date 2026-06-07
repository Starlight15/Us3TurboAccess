#include "client/src/transports/ucx/ucx_endpoint.h"

#include <chrono>

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

void UcxEndpointOps::CloseEpGraceful(
    ucp_worker_h worker, ucp_ep_h ep,
    std::chrono::milliseconds graceful_timeout) noexcept {
  if (!ep) return;

  void* req = ucp_ep_close_nb(ep, UCP_EP_CLOSE_MODE_FLUSH);

  if (UCS_PTR_IS_PTR(req)) {
    const auto deadline =
        std::chrono::steady_clock::now() + graceful_timeout;
    while (ucp_request_check_status(req) == UCS_INPROGRESS) {
      if (std::chrono::steady_clock::now() >= deadline) {
        ucp_request_free(req);
        req = ucp_ep_close_nb(ep, UCP_EP_CLOSE_MODE_FORCE);
        break;
      }
      ucp_worker_progress(worker);
    }
    if (UCS_PTR_IS_PTR(req)) ucp_request_free(req);
  }
}

// ucp_put_nbx 写数据到 gateway gw_raddr，PUT 完成后直接发 AM_WRITE_DONE。
// UCX 同 ep 上 AM 在 PUT 后发出，保证按序到达，无需额外 flush。
std::future<ucs_status_t> UcxEndpointOps::PutAndNotify(
    ucp_worker_h /*worker*/, ucp_ep_h ep,
    const void* src_buf, std::size_t size,
    std::uint64_t gw_raddr, ucp_rkey_h gw_rkey,
    const std::string& session_id) {
  struct Ctx {
    std::promise<ucs_status_t> promise;
    ucp_ep_h    ep;
    std::string session_id;
  };
  auto* ctx       = new Ctx{};
  ctx->ep         = ep;
  ctx->session_id = session_id;
  auto fut        = ctx->promise.get_future();

  // 发 AM_WRITE_DONE 的辅助 lambda（避免代码重复）
  auto send_am = [](Ctx* c) {
    ucp_request_param_t am{};
    am.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                      UCP_OP_ATTR_FIELD_USER_DATA |
                      UCP_OP_ATTR_FIELD_FLAGS;
    am.flags       = UCP_AM_SEND_FLAG_REPLY;
    am.cb.send     = [](void* req, ucs_status_t st, void* ud) {
      ucp_request_free(req);
      static_cast<Ctx*>(ud)->promise.set_value(st);
      delete static_cast<Ctx*>(ud);
    };
    am.user_data   = c;
    void* req = ucp_am_send_nbx(
        c->ep, kAmIdWriteDone,
        c->session_id.c_str(), c->session_id.size(),
        nullptr, 0, &am);
    if (req == nullptr) { c->promise.set_value(UCS_OK);               delete c; }
    else if (UCS_PTR_IS_ERR(req)) { c->promise.set_value(UCS_PTR_STATUS(req)); delete c; }
    // else: pending，callback 接管
  };

  // PUT 完成回调：直接发 AM_WRITE_DONE（无 flush）
  ucp_request_param_t put{};
  put.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  put.cb.send      = [](void* req, ucs_status_t status, void* user_data) {
    ucp_request_free(req);
    auto* c = static_cast<Ctx*>(user_data);
    if (status != UCS_OK) { c->promise.set_value(status); delete c; return; }
    // PUT 完成，直接发 AM
    auto send_am_inner = [](Ctx* fc) {
      ucp_request_param_t am{};
      am.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                        UCP_OP_ATTR_FIELD_USER_DATA |
                        UCP_OP_ATTR_FIELD_FLAGS;
      am.flags       = UCP_AM_SEND_FLAG_REPLY;
      am.cb.send     = [](void* areq, ucs_status_t ast, void* aud) {
        ucp_request_free(areq);
        static_cast<Ctx*>(aud)->promise.set_value(ast);
        delete static_cast<Ctx*>(aud);
      };
      am.user_data   = fc;
      void* areq = ucp_am_send_nbx(
          fc->ep, kAmIdWriteDone,
          fc->session_id.c_str(), fc->session_id.size(),
          nullptr, 0, &am);
      if (areq == nullptr) { fc->promise.set_value(UCS_OK); delete fc; }
      else if (UCS_PTR_IS_ERR(areq)) { fc->promise.set_value(UCS_PTR_STATUS(areq)); delete fc; }
    };
    send_am_inner(c);
  };
  put.user_data = ctx;

  void* req = ucp_put_nbx(ep, src_buf, size, gw_raddr, gw_rkey, &put);
  if (req == nullptr) {
    // PUT 立即完成，直接发 AM
    send_am(ctx);
  } else if (UCS_PTR_IS_ERR(req)) {
    ctx->promise.set_value(UCS_PTR_STATUS(req));
    delete ctx;
  }
  // else: pending，callback 接管

  return fut;
}

}  // namespace us3_turbo_access::client
