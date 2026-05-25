#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "client/src/core/common/brpc_channel.h"
#include "client/src/core/contracts/rpc_requests.h"
#include "control_plane.pb.h"
#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

struct PartCompletion {
  std::uint32_t part_number{0};
  std::string   etag;
};

struct StartUploadOptions {
  ObjectId    object;
  std::size_t expected_total_size{0};
  DataPath    data_path{DataPath::kGdsCuObject};
  std::string idempotency_key;
};

struct StartUploadOutcome {
  std::string upload_id;
  std::size_t max_part_size{0};
};

struct CompleteUploadOutcome {
  std::string etag;
  std::string version;
  std::size_t content_length{0};
};

class MetadataClient {
 public:
  explicit MetadataClient(const ClientOptions& options);

  [[nodiscard]] Result<bool> Initialize();
  void Shutdown();
  [[nodiscard]] bool initialized() const;

  [[nodiscard]] Result<us3_turbo_access::gateway::OpenSessionResponse> OpenTransferSession(
      const SessionOpening& request) const;
  [[nodiscard]] Result<ObjectMetadata> HeadObject(const ObjectId& object) const;

  [[nodiscard]] Result<StartUploadOutcome>
    StartUpload(const StartUploadOptions& opts) const;
  [[nodiscard]] Result<CompleteUploadOutcome>
    CompleteUpload(const std::string& upload_id,
                   const std::vector<PartCompletion>& parts) const;
  [[nodiscard]] Result<bool>
    AbortUpload(const std::string& upload_id) const;

 private:
  BrpcChannel channel_;
  std::unique_ptr<us3_turbo_access::gateway::ControlPlaneService_Stub> stub_;
};

}  // namespace us3_turbo_access::client
