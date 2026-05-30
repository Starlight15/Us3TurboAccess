#include "client/src/core/upload/upload_coordinator.h"

#include "client/src/control/metadata_client.h"
#include "client/src/core/client/client_core.h"
#include "client/src/data/http_data_client.h"

namespace us3_turbo_access::client {

// 路由原则：HTTP 与 GDS/RDMA 控制面完全独立。
//   HTTP   → HttpDataClient (HTTP REST)
//   其他   → MetadataClient  (baidu_std)

Result<UploadCoordinator::StartResult> UploadCoordinator::StartUpload(
    DataPath data_path, const ObjectId& object,
    std::size_t expected_total_size, const std::string& idempotency_key) {
  if (data_path == DataPath::kHttpTcp) {
    // ---- HTTP 独立路径 ----
    auto out = core_.http_data_client().StartUpload(
        object, static_cast<std::uint64_t>(expected_total_size),
        idempotency_key);
    if (!out.success()) {
      return Result<StartResult>::Failure(out.error());
    }
    StartResult r;
    r.upload_id     = std::move(out.value().upload_id);
    r.max_part_size = out.value().max_part_size;
    return Result<StartResult>::Success(std::move(r));
  }

  // ---- GDS / RDMA 独立路径 ----
  StartUploadOptions opts;
  opts.object              = object;
  opts.expected_total_size = expected_total_size;
  opts.data_path           = data_path;
  opts.idempotency_key     = idempotency_key;
  auto out = core_.metadata_client().StartUpload(opts);
  if (!out.success()) {
    return Result<StartResult>::Failure(out.error());
  }
  StartResult r;
  r.upload_id     = std::move(out.value().upload_id);
  r.max_part_size = out.value().max_part_size;
  return Result<StartResult>::Success(std::move(r));
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
  auto out = core_.metadata_client().CompleteUpload(upload_id, control_parts);
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
  return core_.metadata_client().AbortUpload(upload_id);
}

}  // namespace us3_turbo_access::client
