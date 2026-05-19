#pragma once

#include <memory>

#include "client/src/core/common/brpc_channel.h"
#include "client/src/core/contracts/rpc_requests.h"
#include "control_plane.pb.h"
#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

class MetadataClient {
 public:
  explicit MetadataClient(const ClientOptions& options);

  [[nodiscard]] Result<bool> Initialize();
  void Shutdown();
  [[nodiscard]] bool initialized() const;

  [[nodiscard]] Result<fusion_access::gateway::NegotiateTransferSessionResponse> OpenTransferSession(
      const OpenSessionRequest& request) const;
  [[nodiscard]] Result<ObjectMetadata> HeadObject(const ObjectId& object) const;

 private:
  BrpcChannel channel_;
  std::unique_ptr<fusion_access::gateway::ControlPlaneService_Stub> stub_;
};

}  // namespace us3_turbo_access::client
