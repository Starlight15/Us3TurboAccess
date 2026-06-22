#pragma once

#include <memory>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

// 组件类型的完整定义全部收敛到 client_core.cpp 的 ClientCore::Impl 中；
// 这里只前向声明，避免把 cuObject / UCX / 各类 client 的重型 header 泄漏给
// 所有 #include 本文件的 TU。PlatformCapabilities 由 options.h→types.h 提供。
class MetadataClient;
class TransferRouter;
class GdsDataClient;
class GdsMemoryRegistry;
class CuObjectClient;
class GdsTransferPath;
class RdmaTransferPath;
class UploadCoordinator;
class ClientExecutor;

// ClientCore 是内部装配体：拥有所有组件实例并管理生命周期，仅供 Client 使用。
class ClientCore {
 public:
  explicit ClientCore(ClientOptions options);
  // Impl 在本 header 中不完整，析构必须在 .cpp（Impl 完整可见处）定义。
  ~ClientCore();

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
  // 分片上传协调器
  [[nodiscard]] UploadCoordinator& upload_coordinator();
  // 异步操作线程池
  [[nodiscard]] ClientExecutor& async_executor() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace us3_turbo_access::client
