#pragma once

#include <memory>

#include "client/src/core/common/brpc_channel.h"
#include "client/src/core/contracts/rpc_requests.h"
#include "control_plane.pb.h"
#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

class ChannelRegistry;

class GdsDataClient {
 public:
  /** 接受共享 baidu_std Channel；ChannelRegistry 提供。 */
  GdsDataClient(ChannelRegistry& registry, const ClientOptions& options);

  [[nodiscard]] Result<bool> Initialize();
  void Shutdown();
  [[nodiscard]] bool initialized() const;

  [[nodiscard]] Result<us3_turbo_access::gateway::GdsChunkResponse> GdsChunk(
      const ChunkOp& request) const;

  [[nodiscard]] const ClientOptions& options() const noexcept { return options_; }

 private:
  ChannelRegistry&     registry_;
  const ClientOptions& options_;
  std::unique_ptr<us3_turbo_access::gateway::ControlPlaneService_Stub> stub_;
};

}  // namespace us3_turbo_access::client
