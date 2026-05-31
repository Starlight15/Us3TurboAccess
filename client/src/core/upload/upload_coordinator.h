#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "us3_turbo_access/client/result.h"

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
 *
 * Server 隔离：A.1 后 server 端 MultipartCoordinator 在 Complete/Abort 强校验
 * upload->data_path 与调用方 data_path 必须一致；跨通路调用（例如 HTTP REST
 * 试图 Complete 一个 GDS 启动的 upload）会被 server 端 kBadRequest 拒绝。
 * Server 端 upload_id 命名空间是共享的（同一 MultipartStore），但通过 data_path
 * 校验保证使用语义上的隔离。
 *
 * 数据面（UploadPart 的字节传输）仍走 TransferPath::PutObjectPart，
 * Coordinator 不涉及。
 */
class UploadCoordinator {
 public:
  struct StartResult {
    std::string upload_id;
    std::size_t max_part_size{0};
  };

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
   * StartUpload: 按 data_path 分发到 HttpDataClient 或 MetadataClient。
   * @param data_path     选择控制面通道（kHttpTcp 走 HTTP，其它走 baidu_std）
   * @param bucket/key    对象标识
   * @param expected_size 0 表示未知；非 0 让 server 端可预估
   * @param idempotency_key 用户透传的去重 key
   */
  [[nodiscard]] Result<StartResult>
    StartUpload(DataPath data_path, const ObjectId& object,
                std::size_t expected_total_size,
                const std::string& idempotency_key);

  /**
   * CompleteUpload: 同样按 data_path 分发。
   * HTTP 走 /v1/uploads/{id}/complete；其它走 baidu_std CompleteUpload。
   */
  [[nodiscard]] Result<CompleteResult>
    CompleteUpload(DataPath data_path, const std::string& upload_id,
                   const std::vector<PartRef>& parts);

  /**
   * AbortUpload: 同样按 data_path 分发；best-effort（用于析构兜底 / Complete 失败）。
   */
  [[nodiscard]] Result<bool>
    AbortUpload(DataPath data_path, const std::string& upload_id);

 private:
  ClientCore& core_;
};

}  // namespace us3_turbo_access::client
