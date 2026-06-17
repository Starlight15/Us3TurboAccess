#include "client/src/core/client/client_core.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

// ============================================================
//  初始化：按依赖顺序启动所有组件
// ============================================================

Result<bool> ClientCore::Initialize() {
  if (impl_->initialized) {
    return Result<bool>::Success(true);
  }
  if (impl_->options.endpoint.empty()) {
    return Result<bool>::Failure(MakeInvalidArgument("endpoint must not be empty"));
  }

  // 兜底初始化 channels（构造期若 endpoint 已设则已 init；为空时这里再 init）。
  // 三个 baidu_std client 持有的是 channel 指针；只要 channels.Initialize()
  // 成功就同时让 metadata/gds/rdma 三个 client 的 channel_ 有效。
  if (!impl_->channels.ready()) {
    auto ch_init = impl_->channels.Initialize();
    if (!ch_init.success()) return ch_init;
  }

  auto control_init = impl_->metadata_client.Initialize();
  if (!control_init.success()) return control_init;
  auto gds_init = impl_->gds_data_client.Initialize();
  if (!gds_init.success()) return gds_init;
  auto rdma_dp_init = impl_->rdma_data_plane_client.Initialize();
  if (!rdma_dp_init.success()) return rdma_dp_init;

  // HTTP 是可选 fallback 路径：channel 初始化失败也不让整体 init 失败，
  // 仅把 http_executor.available() 留作 false，路径切到 HTTP 时再报错。
  auto http_init = impl_->http_data_client.Initialize();
  impl_->http_executor.SetAvailable(http_init.success());

  // UCX 路径：RdmaTransferPath 构造时已内部初始化 UCX context/worker。
  // available() 内部校验 ucx_ctx_/ucx_worker_ 非空，初始化失败时自动返回 false。
  impl_->rdma_executor.SetAvailable(true);

  // *Async API 用的 worker 线程池：默认 hardware_concurrency 折半（至少 1）。
  std::size_t workers = impl_->options.async_worker_threads;
  if (workers == 0) {
    workers = std::max<std::size_t>(1U, std::thread::hardware_concurrency() / 2U);
  }
  impl_->async_executor = std::make_unique<ClientExecutor>(workers);

  // UploadCoordinator 不持有 I/O 资源，仅依赖 metadata_client + http_data_client；
  // 在它们 Initialize 之后构造即可。
  impl_->upload_coordinator = std::make_unique<UploadCoordinator>(*this);

  impl_->initialized = true;
  return Result<bool>::Success(true);
}

// ============================================================
//  关闭：逆依赖序释放资源
// ============================================================

void ClientCore::Shutdown() {
  // 先显式 Shutdown(grace=5s) 让 executor 排空 inflight bthread，再 reset。
  // C.1 后：grace 内排空返 true → 安全；超时返 false → 累计 unclean_shutdowns
  // 计数，调用方可以查 async_executor().unclean_shutdowns() 排查。
  if (impl_->async_executor) {
    (void)impl_->async_executor->Shutdown(std::chrono::seconds(5));
  }
  impl_->async_executor.reset();
  impl_->upload_coordinator.reset();
  impl_->metadata_client.Shutdown();
  impl_->gds_data_client.Shutdown();
  impl_->rdma_data_plane_client.Shutdown();
  impl_->http_executor.SetAvailable(false);
  impl_->http_data_client.Shutdown();
  impl_->rdma_executor.SetAvailable(false);
  impl_->initialized = false;
}

}  // namespace us3_turbo_access::client
