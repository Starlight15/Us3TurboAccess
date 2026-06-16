#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "backend/backend.h"
#include "core/multipart/multipart_coordinator.h"
#include "control_plane.pb.h"

namespace us3_turbo_access::gateway::api {

/// proto StartUploadRequest → StartUploadParams
/// expected_total_size == 0 视为"未知"，转为 nullopt（与原代码一致）
[[nodiscard]] core::multipart::StartUploadParams
ToStartUploadParams(const ::us3_turbo_access::gateway::StartUploadRequest& req);

/// proto repeated PartEtag → vector<PartRecord>
[[nodiscard]] std::vector<backend::PartRecord>
ToPartRecords(const google::protobuf::RepeatedPtrField<
                  ::us3_turbo_access::gateway::PartEtag>& parts);

/// JSON array of {"part_number": N, "etag": "..."} → vector<PartRecord>
/// 调用方负责 try/catch，本函数不捕获 json 异常（与原代码职责划分一致）
[[nodiscard]] std::vector<backend::PartRecord>
ToPartRecords(const nlohmann::json& parts_array);

}  // namespace us3_turbo_access::gateway::api
