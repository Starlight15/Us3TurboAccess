#include "client/src/core/client/client_core.h"

#include <algorithm>
#include <chrono>
#include <thread>

// ---- Impl 需要的完整类型定义（从 header 下沉到此，避免泄漏给使用者）----
#include "client/src/control/metadata_client.h"
#include "client/src/core/async/client_executor.h"
#include "client/src/core/client/capability_probe.h"
#include "client/src/core/common/channel_registry.h"
#include "client/src/core/common/errors.h"
#include "client/src/core/gds/gds_context.h"
#include "client/src/core/gds/gds_transfer_path.h"
#include "client/src/core/rdma/rdma_transfer_path.h"
#include "client/src/core/routing/transfer_router.h"
#include "client/src/core/upload/upload_coordinator.h"
#include "client/src/data/gds_data_client.h"
#include "client/src/data/rdma_data_plane_client.h"
#include "client/src/transports/gds/cuobject_client.h"
#include "client/src/transports/gds/gds_memory_registry.h"

namespace us3_turbo_access::client {

// ============================================================
//  ClientCore::Impl — 内部组件装配
// ============================================================

struct ClientCore::Impl {
  /* HttpTransferPath 需要并发 GET 的 worker 池；它在 Initialize 才被创建，
   * 而 Impl 构造期 async_executor 还是空。这里给一个稳定的 functor，每次
   * 调用直接从 Impl 取最新 async_executor 指针，未就绪自动降级单连接。*/
  struct AsyncExecutorAccessor {
    Impl* impl;
    ClientExecutor* operator()() const { return impl->async_executor.get(); }
  };

  explicit Impl(ClientOptions opts)
      : options(std::move(opts)),
        caps(DetectPlatformCapabilities(options)),
        channels(options),
        metadata_client(channels, options),
        gds_data_client(channels, options),
        rdma_data_plane_client(channels, options),
        gds_executor(caps, GdsContext{.options = options,
                                      .metadata_client = metadata_client,
                                      .data_client = gds_data_client,
                                      .memory_registry = gds_memory_registry,
                                      .cuobj_client = cuobject_client},
                      AsyncExecutorAccessor{this}),
        rdma_executor(options, metadata_client, rdma_data_plane_client),
        transfer_router(options.data_flow, gds_executor, rdma_executor) {}

  ClientOptions       options;
  PlatformCapabilities caps;
  ChannelRegistry     channels;
  MetadataClient      metadata_client;
  GdsDataClient       gds_data_client;
  RdmaDataPlaneClient rdma_data_plane_client;
  GdsMemoryRegistry   gds_memory_registry;
  CuObjectClient      cuobject_client;
  GdsTransferPath     gds_executor;
  RdmaTransferPath    rdma_executor;
  TransferRouter      transfer_router;
  // upload_coordinator 必须在 metadata_client + http_data_client 之后构造
  // （它持有它们的引用），但又在 async_executor 之前；声明顺序自然满足。
  std::unique_ptr<UploadCoordinator> upload_coordinator;
  // async_executor 必须声明在依赖项之后：析构按声明逆序，先析构 executor
  // 才能 join 完所有 worker，避免 worker 在 transfer_router 等已销毁后访问。
  std::unique_ptr<ClientExecutor> async_executor;
  bool                initialized{false};
};

// ============================================================
//  构造 / 析构 + 访问器（定义在 Impl 完整可见之后）
// ============================================================

ClientCore::ClientCore(ClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

ClientCore::~ClientCore() = default;

bool ClientCore::initialized() const { return impl_->initialized; }
const PlatformCapabilities& ClientCore::capabilities() const { return impl_->caps; }
const ClientOptions& ClientCore::options() const { return impl_->options; }
const MetadataClient& ClientCore::metadata_client() const { return impl_->metadata_client; }
const TransferRouter& ClientCore::transfer_router() const { return impl_->transfer_router; }
GdsDataClient& ClientCore::gds_data_client() { return impl_->gds_data_client; }
GdsMemoryRegistry& ClientCore::gds_memory_registry() { return impl_->gds_memory_registry; }
const CuObjectClient& ClientCore::cuobj_client() const { return impl_->cuobject_client; }
const GdsTransferPath& ClientCore::gds_transfer_path() const { return impl_->gds_executor; }
const RdmaTransferPath& ClientCore::rdma_transfer_path() const { return impl_->rdma_executor; }
UploadCoordinator& ClientCore::upload_coordinator() { return *impl_->upload_coordinator; }
ClientExecutor& ClientCore::async_executor() const { return *impl_->async_executor; }

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
  impl_->rdma_executor.SetAvailable(false);
  impl_->initialized = false;
}

}  // namespace us3_turbo_access::client
