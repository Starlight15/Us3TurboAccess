#include "client/src/core/upload/upload_coordinator.h"

#include "client/src/control/metadata_client.h"
#include "client/src/core/client/client_core.h"
#include "client/src/core/gds/gds_transfer_path.h"
#include "client/src/core/http/http_transfer_path.h"
#include "client/src/core/rdma/rdma_transfer_path.h"
#include "client/src/data/http_data_client.h"

namespace us3_turbo_access::client {

// 路由原则：HTTP 与 GDS/RDMA 控制面完全独立。
//   HTTP   → HttpDataClient (HTTP REST)
//   其他   → MetadataClient  (baidu_std)

// 职责：按 data_path 路由控制面；不含参数转换或业务逻辑。
Result<StartUploadResult> UploadCoordinator::RouteStartUpload(
    const ObjectDescriptor& desc) {
  if (desc.data_path == DataPath::kHttpTcp) {
    // ---- HTTP 独立路径 ----
    auto out = core_.http_data_client().StartUpload(
        desc.object,
        static_cast<std::uint64_t>(desc.expected_total_size.value_or(0)),
        desc.idempotency_key);
    if (!out.success()) {
      return Result<StartUploadResult>::Failure(out.error());
    }
    StartUploadResult r;
    r.upload_id     = std::move(out.value().upload_id);
    r.max_part_size = out.value().max_part_size;
    return Result<StartUploadResult>::Success(std::move(r));
  }

  // ---- GDS / RDMA 独立路径 ----
  return core_.metadata_client().RpcStartUpload(desc);
}

Result<UploadCoordinator::CompleteResult> UploadCoordinator::CompleteUpload(
    DataPath data_path, const std::string& upload_id,
    const std::vector<PartRef>& parts) {
  if (data_path == DataPath::kHttpTcp) {
    // ---- HTTP 独立路径 ----
    std::vector<HttpDataClient::PartEtag> http_parts;
    http_parts.reserve(parts.size());
    for (const auto& p : parts) {
      http_parts.push_back({p.part_number, p.etag, std::nullopt});
    }
    auto out = core_.http_data_client().CompleteUpload(upload_id, http_parts);
    if (!out.success()) {
      return Result<CompleteResult>::Failure(out.error());
    }
    CompleteResult r;
    r.etag           = out.value().etag;
    r.version        = out.value().version;
    r.content_length = out.value().content_length;
    return Result<CompleteResult>::Success(std::move(r));
  }

  // ---- GDS / RDMA 独立路径 ----
  std::vector<PartCompletion> control_parts;
  control_parts.reserve(parts.size());
  for (const auto& p : parts) {
    control_parts.push_back({p.part_number, p.etag});
  }
  auto out = core_.metadata_client().CompleteUpload(upload_id, control_parts, data_path);
  if (!out.success()) {
    return Result<CompleteResult>::Failure(out.error());
  }
  CompleteResult r;
  r.etag           = out.value().etag;
  r.version        = out.value().version;
  r.content_length = out.value().content_length;
  return Result<CompleteResult>::Success(std::move(r));
}

Result<bool> UploadCoordinator::AbortUpload(DataPath data_path,
                                            const std::string& upload_id) {
  if (data_path == DataPath::kHttpTcp) {
    return core_.http_data_client().AbortUpload(upload_id);
  }
  return core_.metadata_client().AbortUpload(upload_id, data_path);
}

// 数据面收敛：三通路差异：
//   HTTP/RDMA: length=buffer.size 让 server 预分配 buffer
//   GDS:       length 留空（保留 expected_size=0 跳过整对象 Reserve 的行为）
Result<TransferOutcome> UploadCoordinator::UploadPart(
    const ObjectDescriptor& desc,
    const std::string& upload_id, std::uint32_t part_number,
    ConstBufferView buffer) {
  RequestOptions request;
  request.object          = desc.object;
  request.offset          = desc.offset.value_or(0);
  request.checksum_policy = desc.checksum_policy;

  switch (desc.data_path) {
    case DataPath::kHttpTcp:
      request.length = buffer.size;
      return core_.http_transfer_path().PutObjectPart(request, buffer,
                                                       upload_id, part_number);
    case DataPath::kNativeRdma:
      request.length = buffer.size;  // 让 server 端 BindSession 阶段能分配 buffer
      return core_.rdma_transfer_path().PutObjectPart(request, buffer,
                                                       upload_id, part_number);
    case DataPath::kGdsCuObject:
    default:
      // GDS: 不设 length，保留 expected_size=0 跳过整对象 Reserve。
      return core_.gds_transfer_path().PutObjectPart(request, buffer,
                                                      upload_id, part_number);
  }
}

}  // namespace us3_turbo_access::client
