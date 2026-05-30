#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "client/src/core/common/brpc_channel.h"
#include "rdma_data_plane.pb.h"
#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

struct RdmaDiscoverInfo {
  std::string   host;
  std::uint32_t port{0};
  std::uint64_t max_msg_bytes{0};
};

struct RdmaBindInfo {
  std::uint64_t raddr{0};
  std::uint32_t rkey{0};
};

struct RdmaCommitInfo {
  std::string etag;
  std::string version;
};

struct RdmaCommitPartInfo {
  std::string part_etag;
};

class ChannelRegistry;

/**
 * 客户端调 RdmaDataPlaneService 的薄 stub。
 * 与 MetadataClient / GdsDataClient 平行；本期共用 ChannelRegistry 的 baidu_std Channel。
 */
class RdmaDataPlaneClient {
 public:
  /** 接受共享 baidu_std Channel；ChannelRegistry 提供。 */
  RdmaDataPlaneClient(ChannelRegistry& registry, const ClientOptions& options);

  [[nodiscard]] Result<bool> Initialize();
  void Shutdown();
  [[nodiscard]] bool initialized() const;

  [[nodiscard]] Result<RdmaDiscoverInfo>
    DiscoverEndpoint(const std::string& session_id) const;

  [[nodiscard]] Result<RdmaBindInfo>
    BindSessionToConnection(const std::string& session_id,
                            std::uint64_t conn_token) const;

  [[nodiscard]] Result<RdmaCommitInfo>
    CommitObject(const std::string& session_id,
                 std::uint64_t bytes_transferred) const;

  [[nodiscard]] Result<RdmaCommitPartInfo>
    CommitPart(const std::string& session_id, const std::string& upload_id,
               std::uint32_t part_number, std::uint64_t bytes_transferred) const;

  /** PUT 失败路径调用：让 gateway 立即释放该 session 的 buffer/MR。Best-effort。*/
  [[nodiscard]] Result<bool>
    AbortSession(const std::string& session_id) const;

  [[nodiscard]] const ClientOptions& options() const noexcept { return options_; }

 private:
  ChannelRegistry&     registry_;
  const ClientOptions& options_;
  std::unique_ptr<us3_turbo_access::gateway::RdmaDataPlaneService_Stub> stub_;
};

}  // namespace us3_turbo_access::client
