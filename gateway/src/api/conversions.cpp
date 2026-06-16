#include "api/conversions.h"

#include <cstddef>
#include <utility>

namespace us3_turbo_access::gateway::api {

[[nodiscard]] core::multipart::StartUploadParams
ToStartUploadParams(const ::us3_turbo_access::gateway::StartUploadRequest& req) {
  core::multipart::StartUploadParams params;
  params.bucket          = req.bucket();
  params.object_key      = req.object_key();
  if (req.expected_total_size() != 0U) {
    params.expected_total_size =
        static_cast<std::size_t>(req.expected_total_size());
  }
  params.data_path       = req.data_path();
  params.idempotency_key = req.idempotency_key();
  return params;
}

[[nodiscard]] std::vector<backend::PartRecord>
ToPartRecords(const google::protobuf::RepeatedPtrField<
                  ::us3_turbo_access::gateway::PartEtag>& parts) {
  std::vector<backend::PartRecord> result;
  result.reserve(static_cast<std::size_t>(parts.size()));
  for (const auto& p : parts) {
    backend::PartRecord pr;
    pr.part_number = p.part_number();
    pr.etag = p.etag();
    result.push_back(std::move(pr));
  }
  return result;
}

[[nodiscard]] std::vector<backend::PartRecord>
ToPartRecords(const nlohmann::json& parts_array) {
  std::vector<backend::PartRecord> result;
  for (const auto& p : parts_array) {
    backend::PartRecord r;
    r.part_number = p.at("part_number").get<std::uint32_t>();
    r.etag        = p.at("etag").get<std::string>();
    // offset/size：CompleteUpload 服务端用 RegisterPart 已经登记过 size，
    // 这里只校验 part_number + etag 一致即可，offset/size 留 0 让后端按记录值算。
    result.push_back(std::move(r));
  }
  return result;
}

}  // namespace us3_turbo_access::gateway::api
