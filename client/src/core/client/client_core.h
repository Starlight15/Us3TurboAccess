#pragma once

#include <memory>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

// ---- Impl 需要的完整类型定义 ----
#include "client/src/control/metadata_client.h"
#include "client/src/core/async/client_executor.h"
#include "client/src/core/client/capability_probe.h"
#include "client/src/core/common/channel_registry.h"
#include "client/src/core/gds/gds_context.h"
#include "client/src/core/gds/gds_transfer_path.h"
#include "client/src/core/http/http_transfer_path.h"
#include "client/src/core/rdma/rdma_transfer_path.h"
#include "client/src/core/routing/transfer_router.h"
#include "client/src/core/upload/upload_coordinator.h"
#include "client/src/data/gds_data_client.h"
#include "client/src/data/http_data_client.h"
#include "client/src/data/rdma_data_plane_client.h"
#include "client/src/transports/gds/cuobject_client.h"
#include "client/src/transports/gds/gds_memory_registry.h"

namespace us3_turbo_access::client {

// ClientCore 是内部装配体：拥有所有组件实例并管理生命周期，仅供 Client 使用。
class ClientCore {
 public:
  explicit ClientCore(ClientOptions options);
  ~ClientCore() = default;

  ClientCore(const ClientCore&) = delete;
  ClientCore& operator=(const ClientCore&) = delete;
  ClientCore(ClientCore&&) = delete;
  ClientCore& operator=(ClientCore&&) = delete;

  // 初始化所有组件（幂等），必须在传输操作前调用
  [[nodiscard]] Result<bool> Initialize();
  // 关闭所有组件并释放资源，不可重新初始化
  void Shutdown();
  // 是否已完成 Initialize
  [[nodiscard]] bool initialized() const;
  // 运行时检测的平台能力（GDS/RDMA 可用性等）
  [[nodiscard]] const PlatformCapabilities& capabilities() const;
  // 客户端配置选项（只读）
  [[nodiscard]] const ClientOptions& options() const;
  // 控制面元数据 RPC 客户端
  [[nodiscard]] const MetadataClient& metadata_client() const;
  // 数据通路路由器
  [[nodiscard]] const TransferRouter& transfer_router() const;
  // GDS 数据面 RPC 客户端
  [[nodiscard]] GdsDataClient& gds_data_client();
  // GPU buffer 注册表（cuObj descriptor 管理）
  [[nodiscard]] GdsMemoryRegistry& gds_memory_registry();
  // cuObj 客户端（RDMA 操作封装）
  [[nodiscard]] const CuObjectClient& cuobj_client() const;
  // GDS 传输通路
  [[nodiscard]] const GdsTransferPath& gds_transfer_path() const;
  // UCX RDMA 传输通路
  [[nodiscard]] const RdmaTransferPath& rdma_transfer_path() const;
  // HTTP 传输通路
  [[nodiscard]] const HttpTransferPath& http_transfer_path() const;
  // HTTP 数据面客户端
  [[nodiscard]] HttpDataClient& http_data_client();
  // 分片上传协调器
  [[nodiscard]] UploadCoordinator& upload_coordinator();
  // 异步操作线程池
  [[nodiscard]] ClientExecutor& async_executor() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

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
        http_data_client(options),
        gds_executor(caps, GdsContext{.options = options,
                                      .metadata_client = metadata_client,
                                      .data_client = gds_data_client,
                                      .memory_registry = gds_memory_registry,
                                      .cuobj_client = cuobject_client},
                      AsyncExecutorAccessor{this}),
        rdma_executor(options, metadata_client, rdma_data_plane_client),
        http_executor(options, http_data_client,
                       AsyncExecutorAccessor{this}),
        transfer_router(options.data_path, gds_executor, rdma_executor,
                        http_executor) {}

  ClientOptions       options;
  PlatformCapabilities caps;
  // ChannelRegistry 必须在三个 baidu_std client 之前构造（提供 brpc::Channel*）。
  // 真正的 brpc::Channel 在 ChannelRegistry::Initialize 时才创建；构造期
  // baidu_std() 返回 nullptr，三个 client 接受 nullptr，等 Initialize 路径
  // 把 channel 建好后再调它们各自的 Initialize 创建 Stub。
  // 由于 client 持的是 brpc::Channel*（成员变量在 Impl 构造时已绑定），
  // ChannelRegistry::Initialize 必须创建 channel 并把 unique_ptr 持有，
  // 而 baidu_std() 返回的是 unique_ptr.get()——因此 client 持的指针必须在
  // Initialize 后才有效。把 ChannelRegistry::Initialize 放在最早一步即可。
  ChannelRegistry     channels;
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
  // upload_coordinator 必须在 metadata_client + http_data_client 之后构造
  // （它持有它们的引用），但又在 async_executor 之前；声明顺序自然满足。
  std::unique_ptr<UploadCoordinator> upload_coordinator;
  // async_executor 必须声明在依赖项之后：析构按声明逆序，先析构 executor
  // 才能 join 完所有 worker，避免 worker 在 transfer_router 等已销毁后访问。
  std::unique_ptr<ClientExecutor> async_executor;
  bool                initialized{false};
};

// ============================================================
//  内联构造函数 + 访问器（定义在 Impl 完整可见之后）
// ============================================================

inline ClientCore::ClientCore(ClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

inline bool ClientCore::initialized() const { return impl_->initialized; }
inline const PlatformCapabilities& ClientCore::capabilities() const { return impl_->caps; }
inline const ClientOptions& ClientCore::options() const { return impl_->options; }
inline const MetadataClient& ClientCore::metadata_client() const { return impl_->metadata_client; }
inline const TransferRouter& ClientCore::transfer_router() const { return impl_->transfer_router; }
inline GdsDataClient& ClientCore::gds_data_client() { return impl_->gds_data_client; }
inline GdsMemoryRegistry& ClientCore::gds_memory_registry() { return impl_->gds_memory_registry; }
inline const CuObjectClient& ClientCore::cuobj_client() const { return impl_->cuobject_client; }
inline const GdsTransferPath& ClientCore::gds_transfer_path() const { return impl_->gds_executor; }
inline const RdmaTransferPath& ClientCore::rdma_transfer_path() const { return impl_->rdma_executor; }
inline const HttpTransferPath& ClientCore::http_transfer_path() const { return impl_->http_executor; }
inline HttpDataClient& ClientCore::http_data_client() { return impl_->http_data_client; }
inline UploadCoordinator& ClientCore::upload_coordinator() { return *impl_->upload_coordinator; }
inline ClientExecutor& ClientCore::async_executor() const { return *impl_->async_executor; }

}  // namespace us3_turbo_access::client
