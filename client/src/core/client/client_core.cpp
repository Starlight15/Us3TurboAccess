#include "client/src/core/client/client_core.h"

#include <algorithm>
#include <thread>

#include "client/src/control/metadata_client.h"
#include "client/src/core/async/client_executor.h"
#include "client/src/core/client/capability_probe.h"
#include "client/src/core/common/errors.h"
#include "client/src/core/gds/gds_context.h"
#include "client/src/core/gds/gds_transfer_path.h"
#include "client/src/core/http/http_transfer_path.h"
#include "client/src/core/rdma/rdma_transfer_path.h"
#include "client/src/core/routing/transfer_router.h"
#include "client/src/data/gds_data_client.h"
#include "client/src/data/http_data_client.h"
#include "client/src/data/rdma_data_plane_client.h"
#include "client/src/transports/gds/cuobject_client.h"
#include "client/src/transports/gds/gds_memory_registry.h"
#include "client/src/transports/rdma/rdma_resources.h"

namespace us3_turbo_access::client {

struct ClientCore::Impl {
  explicit Impl(ClientOptions opts)
      : options(std::move(opts)),
        caps(DetectPlatformCapabilities(options)),
        metadata_client(options),
        gds_data_client(options),
        rdma_data_plane_client(options),
        http_data_client(options),
        gds_executor(caps, GdsContext{.options = options,
                                      .metadata_client = metadata_client,
                                      .data_client = gds_data_client,
                                      .memory_registry = gds_memory_registry,
                                      .cuobj_client = cuobject_client}),
        rdma_executor(options, metadata_client, rdma_data_plane_client),
        // 并发 GET 用 async_executor；构造时它还未初始化（Initialize 才创建），
        // 用 provider lambda 延后取，未就绪自动降级为单连接。
        http_executor(options, http_data_client,
                       [this]() -> ClientExecutor* {
                         return async_executor.get();
                       }),
        transfer_router(options.data_path, gds_executor, rdma_executor,
                        http_executor) {}

  ClientOptions       options;
  PlatformCapabilities caps;
  MetadataClient      metadata_client;
  GdsDataClient       gds_data_client;
  RdmaDataPlaneClient rdma_data_plane_client;
  HttpDataClient      http_data_client;
  GdsMemoryRegistry   gds_memory_registry;
  CuObjectClient      cuobject_client;
  GdsTransferPath     gds_executor;
  RdmaTransferPath    rdma_executor;
  HttpTransferPath    http_executor;
  TransferRouter      transfer_router;
  // async_executor 必须声明在依赖项之后：析构按声明逆序，先析构 executor
  // 才能 join 完所有 worker，避免 worker 在 transfer_router 等已销毁后访问。
  std::unique_ptr<ClientExecutor> async_executor;
  bool                initialized{false};
};

ClientCore::ClientCore(ClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

ClientCore::~ClientCore() = default;

Result<bool> ClientCore::Initialize() {
  if (impl_->initialized) {
    return Result<bool>::Success(true);
  }
  if (impl_->options.endpoint.empty()) {
    return Result<bool>::Failure(MakeInvalidArgument("endpoint must not be empty"));
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

  // 探测 RDMA 设备是否可用：成功就标 available；失败仅让 RDMA path 不 available。
  // 实际 PD/CQ/MR 在每次 PutObject 现场基于 cm_id->verbs 创建，本处只做探活。
  auto resources = RdmaResources::Open(impl_->options.rdma);
  impl_->rdma_executor.SetAvailable(resources.success());
  (void)resources;  // probe-only: discard immediately

  // *Async API 用的 worker 线程池：默认 hardware_concurrency 折半（至少 1）。
  std::size_t workers = impl_->options.async_worker_threads;
  if (workers == 0) {
    workers = std::max<std::size_t>(1U, std::thread::hardware_concurrency() / 2U);
  }
  impl_->async_executor = std::make_unique<ClientExecutor>(workers);

  impl_->initialized = true;
  return Result<bool>::Success(true);
}

void ClientCore::Shutdown() {
  // 先关掉 executor，确保所有 *Async 任务跑完再拆下层组件。
  impl_->async_executor.reset();
  impl_->metadata_client.Shutdown();
  impl_->gds_data_client.Shutdown();
  impl_->rdma_data_plane_client.Shutdown();
  impl_->http_executor.SetAvailable(false);
  impl_->http_data_client.Shutdown();
  impl_->rdma_executor.SetAvailable(false);
  impl_->initialized = false;
}

bool ClientCore::initialized() const { return impl_->initialized; }
const PlatformCapabilities& ClientCore::capabilities() const { return impl_->caps; }
const ClientOptions& ClientCore::options() const { return impl_->options; }
const MetadataClient& ClientCore::metadata_client() const { return impl_->metadata_client; }
const TransferRouter& ClientCore::transfer_router() const { return impl_->transfer_router; }

GdsDataClient& ClientCore::gds_data_client() { return impl_->gds_data_client; }
GdsMemoryRegistry& ClientCore::gds_memory_registry() { return impl_->gds_memory_registry; }
const CuObjectClient& ClientCore::cuobj_client() const { return impl_->cuobject_client; }
const RdmaTransferPath& ClientCore::rdma_transfer_path() const { return impl_->rdma_executor; }
const HttpTransferPath& ClientCore::http_transfer_path() const { return impl_->http_executor; }
HttpDataClient& ClientCore::http_data_client() { return impl_->http_data_client; }
ClientExecutor& ClientCore::async_executor() const { return *impl_->async_executor; }

}  // namespace us3_turbo_access::client
