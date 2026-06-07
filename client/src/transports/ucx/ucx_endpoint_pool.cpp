#include "client/src/transports/ucx/ucx_endpoint_pool.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

UcxEndpointPool::~UcxEndpointPool() {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& [key, q] : idle_) {
    for (EpSlot& s : q) {
      if (s.rkey) ucp_rkey_destroy(s.rkey);
      void* req = ucp_ep_close_nb(s.ep, UCP_EP_CLOSE_MODE_FORCE);
      if (UCS_PTR_IS_PTR(req)) {
        for (int i = 0; i < 100 && ucp_request_check_status(req) == UCS_INPROGRESS; ++i)
          ucp_worker_progress(worker_.handle());
        ucp_request_free(req);
      }
    }
  }
}

Result<EpSlot> UcxEndpointPool::Acquire(
    const std::string& host, uint16_t port,
    const std::vector<std::uint8_t>& packed_rkey) {
  const std::string key = Key(host, port);
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = idle_.find(key);
    if (it != idle_.end() && !it->second.empty()) {
      EpSlot slot = it->second.front();
      it->second.pop_front();
      return Result<EpSlot>::Success(slot);  // ep+rkey 均已缓存
    }
  }

  // 新建 ep
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    return Result<EpSlot>::Failure(
        MakeTransportFailure("invalid UCX host: " + host, DataPath::kNativeRdma, {}, false));

  ucp_ep_params_t params{};
  params.field_mask       = UCP_EP_PARAM_FIELD_FLAGS | UCP_EP_PARAM_FIELD_SOCK_ADDR;
  params.flags            = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
  params.sockaddr.addr    = reinterpret_cast<const sockaddr*>(&addr);
  params.sockaddr.addrlen = sizeof(addr);

  ucp_ep_h ep = nullptr;
  ucs_status_t st = ucp_ep_create(worker_.handle(), &params, &ep);
  if (st != UCS_OK)
    return Result<EpSlot>::Failure(
        MakeTransportFailure(ucs_status_string(st), DataPath::kNativeRdma, {}, true));

  // 新 ep 首次 unpack rkey
  ucp_rkey_h rkey = nullptr;
  if (!packed_rkey.empty()) {
    auto ust = ucp_ep_rkey_unpack(ep, packed_rkey.data(), &rkey);
    if (ust != UCS_OK) rkey = nullptr;  // 失败时 rkey 保持 nullptr
  }

  return Result<EpSlot>::Success(EpSlot{ep, rkey});
}

void UcxEndpointPool::Return(const std::string& host, uint16_t port, EpSlot slot) {
  if (!slot.ep) return;
  // rkey 与 gateway MR 绑定，MR 可能因 gateway 重启而失效，归还时销毁
  // ep 长连接保留，下次使用时重新 unpack 当次 PrepareTransfer 返回的 rkey
  if (slot.rkey) { ucp_rkey_destroy(slot.rkey); slot.rkey = nullptr; }
  const std::string key = Key(host, port);
  std::lock_guard<std::mutex> lk(mu_);
  auto& q = idle_[key];
  if (q.size() < max_idle_) {
    q.push_back(slot);  // 只缓存 ep，rkey 已清空
  } else {
    void* req = ucp_ep_close_nb(slot.ep, UCP_EP_CLOSE_MODE_FORCE);
    if (UCS_PTR_IS_PTR(req)) ucp_request_free(req);
  }
}

void UcxEndpointPool::Discard(EpSlot slot) noexcept {
  if (!slot.ep) return;
  if (slot.rkey) ucp_rkey_destroy(slot.rkey);
  void* req = ucp_ep_close_nb(slot.ep, UCP_EP_CLOSE_MODE_FORCE);
  if (UCS_PTR_IS_PTR(req)) {
    for (int i = 0; i < 100 && ucp_request_check_status(req) == UCS_INPROGRESS; ++i)
      ucp_worker_progress(worker_.handle());
    ucp_request_free(req);
  }
}

}  // namespace us3_turbo_access::client
