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

struct CompleteUploadOutcome {
  std::string etag;
  std::string version;
  std::size_t content_length{0};
};

class ChannelRegistry;

class MetadataClient {
 public:
  MetadataClient(ChannelRegistry& registry, const ClientOptions& options);

  [[nodiscard]] Result<bool> Initialize();
  void Shutdown();
  [[nodiscard]] bool initialized() const;

  [[nodiscard]] Result<us3_turbo_access::gateway::OpenSessionResponse> RpcOpenTransferSession(
      const SessionOpening& request) const;
  [[nodiscard]] Result<ObjectMetadata> HeadObject(const ObjectId& object) const;

  [[nodiscard]] Result<StartUploadResult>
    RpcCreateMultipartUpload(const ObjectDescriptor& desc) const;
  [[nodiscard]] Result<CompleteUploadOutcome>
    RpcCompleteMultipartUpload(const std::string& upload_id,
                               const std::vector<PartCompletion>& parts,
                               DataFlow data_flow) const;
  [[nodiscard]] Result<bool>
    RpcAbortMultipartUpload(const std::string& upload_id,
                            DataFlow data_flow) const;

  /**
   * 通知 server 立即 mark 该 session 为 failed（best-effort）。
   * 用于 GDS PutObject/PutObjectPart 重试前清理旧 session，避免 server 端
   * session 积压（GDS 通路与 RDMA 通路不同，没有专门数据面 abort）。
   * 不存在的 session 视为成功（no-op），返回 erased=false。
   */
  [[nodiscard]] Result<bool>
    AbortSession(const std::string& session_id) const;

  [[nodiscard]] const ClientOptions& options() const noexcept { return options_; }

 private:
  ChannelRegistry&     registry_;
  const ClientOptions& options_;
  std::unique_ptr<us3_turbo_access::gateway::ControlPlaneService_Stub> stub_;
};

}  // namespace us3_turbo_access::client
