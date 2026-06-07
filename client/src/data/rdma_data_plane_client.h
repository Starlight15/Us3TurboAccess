#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "client/src/core/common/brpc_channel.h"
#include "rdma_data_plane.pb.h"
#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

struct RdmaDiscoverInfo {
  std::string              host;
  std::uint32_t            ucx_port{0};
  std::uint64_t            max_msg_bytes{0};
  std::uint64_t            gw_raddr{0};       // gateway buffer 虚地址
  std::vector<std::uint8_t> gw_packed_rkey;   // ucp_rkey_pack 结果
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
 *
 * RMA 模式：PrepareTransfer 携带 client_raddr/packed_rkey/transfer_bytes；
 * gateway 返回 ucx_port，client 据此建 ep 并发 AM_REGISTER。
 * PrepareTransfer 结果不缓存——raddr/rkey 是 per-session 的。
 */
class RdmaDataPlaneClient {
 public:
  RdmaDataPlaneClient(ChannelRegistry& registry, const ClientOptions& options);

  [[nodiscard]] Result<bool> Initialize();
  void Shutdown();
  [[nodiscard]] bool initialized() const;

  /**
   * 通知 gateway 分配 gw_buf 并注册 RMA；返回 gw_raddr/gw_packed_rkey 供 client PUT。
   */
  [[nodiscard]] Result<RdmaDiscoverInfo>
    PrepareTransfer(const std::string& session_id,
                    std::uint64_t transfer_bytes) const;

  [[nodiscard]] Result<RdmaCommitInfo>
    CommitObject(const std::string& session_id,
                 std::uint64_t bytes_transferred,
                 const std::string& client_crc32c_b64 = {}) const;

  [[nodiscard]] Result<RdmaCommitPartInfo>
    CommitPart(const std::string& session_id, const std::string& upload_id,
               std::uint32_t part_number, std::uint64_t bytes_transferred,
               const std::string& client_crc32c_b64 = {}) const;

  [[nodiscard]] Result<bool>
    AbortSession(const std::string& session_id) const;

  [[nodiscard]] const ClientOptions& options() const noexcept { return options_; }

 private:
  ChannelRegistry&     registry_;
  const ClientOptions& options_;
  std::unique_ptr<us3_turbo_access::gateway::RdmaDataPlaneService_Stub> stub_;
};

}  // namespace us3_turbo_access::client
