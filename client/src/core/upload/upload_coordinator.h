#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "us3_turbo_access/client/result.h"
#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

class ClientCore;

/**
 * UploadCoordinator: multipart 控制面路由层。
 *
 * 设计原则：HTTP 与 GDS/RDMA 两条逻辑【完全独立，无元数据控制流的重合】。
 *
 *   HTTP 路径   : HttpDataClient   (HTTP REST: POST /v1/uploads/...)
 *   GDS/RDMA 路径: MetadataClient   (baidu_std: ControlPlaneService)
 *
 * Coordinator 仅按 data_path 透明分发，不试图统一两端协议。
 */
class UploadCoordinator {
 public:
  struct CompleteResult {
    std::string etag;
    std::string version;
    std::size_t content_length{0};
  };

  struct PartRef {
    std::uint32_t part_number{0};
    std::string   etag;
  };

  explicit UploadCoordinator(ClientCore& core) : core_(core) {}

  /**
   * RouteStartUpload: 按 descriptor.data_path 分发到 HttpDataClient 或 MetadataClient。
   * 返回统一的 StartUploadResult（与 MetadataClient::RpcStartUpload 共享同一类型）。
   */
  [[nodiscard]] Result<StartUploadResult>
    RouteStartUpload(const ObjectDescriptor& desc);

  [[nodiscard]] Result<CompleteResult>
    CompleteUpload(DataPath data_path, const std::string& upload_id,
                   const std::vector<PartRef>& parts);

  [[nodiscard]] Result<bool>
    AbortUpload(DataPath data_path, const std::string& upload_id);

  /**
   * UploadPart: 数据面收敛入口。按 desc.data_path 分发到对应 TransferPath::PutObjectPart。
   * 三通路差异（length 语义）在此统一组装 RequestOptions：
   *   - HTTP/RDMA: length = buffer.size
   *   - GDS:       length 留空
   */
  [[nodiscard]] Result<TransferOutcome>
    UploadPart(const ObjectDescriptor& desc,
               const std::string& upload_id, std::uint32_t part_number,
               ConstBufferView buffer);

 private:
  ClientCore& core_;
};

}  // namespace us3_turbo_access::client
