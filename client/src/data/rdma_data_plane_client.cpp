#include "client/src/data/rdma_data_plane_client.h"

#include <brpc/controller.h>

#include "client/src/core/common/brpc_channel.h"
#include "client/src/core/common/channel_registry.h"
#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {

RdmaDataPlaneClient::RdmaDataPlaneClient(ChannelRegistry& registry,
                                          const ClientOptions& options)
    : registry_(registry), options_(options) {}

Result<bool> RdmaDataPlaneClient::Initialize() {
  if (initialized()) return Result<bool>::Success(true);
  auto* ch = registry_.baidu_std();
  if (ch == nullptr) {
    return Result<bool>::Failure(MakeInvalidArgument(
        "RdmaDataPlaneClient: shared baidu_std channel is not initialized"));
  }
  stub_ = std::make_unique<
      us3_turbo_access::gateway::RdmaDataPlaneService_Stub>(ch);
  return Result<bool>::Success(true);
}

void RdmaDataPlaneClient::Shutdown() {
  stub_.reset();
}

bool RdmaDataPlaneClient::initialized() const {
  return stub_ != nullptr;
}

Result<RdmaDiscoverInfo> RdmaDataPlaneClient::PrepareTransfer(
    const std::string& session_id,
    std::uint64_t transfer_bytes) const {
  if (!initialized()) {
    return Result<RdmaDiscoverInfo>::Failure(
        MakeNotInitialized("RDMA data plane client"));
  }

  brpc::Controller controller;
  ApplyRequestTimeout(controller, options_);
  us3_turbo_access::gateway::RdmaDiscoverRequest req;
  req.set_session_id(session_id);
  req.set_transfer_bytes(transfer_bytes);

  us3_turbo_access::gateway::RdmaDiscoverResponse resp;
  stub_->DiscoverRdmaEndpoint(&controller, &req, &resp, nullptr);
  auto status = CheckRpcFailure(controller, "PrepareTransfer RPC failed",
                                DataFlow::CPUDirect, "");
  if (!status.success()) {
    return Result<RdmaDiscoverInfo>::Failure(status.error());
  }

  const std::string& rkey_str = resp.gw_packed_rkey();
  RdmaDiscoverInfo info;
  info.host          = resp.host();
  info.ucx_port      = resp.ucx_port();
  info.max_msg_bytes = resp.max_msg_bytes();
  info.gw_raddr      = resp.gw_raddr();
  info.gw_packed_rkey.assign(
      reinterpret_cast<const std::uint8_t*>(rkey_str.data()),
      reinterpret_cast<const std::uint8_t*>(rkey_str.data()) + rkey_str.size());
  return Result<RdmaDiscoverInfo>::Success(std::move(info));
}

Result<RdmaCommitInfo> RdmaDataPlaneClient::CommitObject(
    const std::string& session_id, std::uint64_t bytes_transferred,
    const std::string& client_crc32c_b64) const {
  if (!initialized()) {
    return Result<RdmaCommitInfo>::Failure(MakeNotInitialized("RDMA data plane client"));
  }
  brpc::Controller controller;
  ApplyRequestTimeout(controller, options_);
  us3_turbo_access::gateway::RdmaCommitRequest req;
  req.set_session_id(session_id);
  req.set_bytes_transferred(bytes_transferred);
  if (!client_crc32c_b64.empty()) {
    req.set_client_checksum(client_crc32c_b64);
  }
  us3_turbo_access::gateway::RdmaCommitResponse resp;
  stub_->CommitObject(&controller, &req, &resp, nullptr);
  auto status = CheckRpcFailure(controller, "CommitObject RPC failed",
                                DataFlow::CPUDirect, "");
  if (!status.success()) {
    return Result<RdmaCommitInfo>::Failure(status.error());
  }
  RdmaCommitInfo info;
  info.etag    = resp.etag();
  info.version = resp.version();
  return Result<RdmaCommitInfo>::Success(std::move(info));
}

Result<bool> RdmaDataPlaneClient::AbortSession(
    const std::string& session_id) const {
  if (!initialized()) {
    return Result<bool>::Failure(MakeNotInitialized("RDMA data plane client"));
  }
  brpc::Controller controller;
  ApplyRequestTimeout(controller, options_);
  us3_turbo_access::gateway::RdmaAbortRequest req;
  req.set_session_id(session_id);
  us3_turbo_access::gateway::RdmaAbortResponse resp;
  stub_->AbortSession(&controller, &req, &resp, nullptr);
  auto status = CheckRpcFailure(controller, "AbortSession RPC failed",
                                DataFlow::CPUDirect, "");
  if (!status.success()) {
    return Result<bool>::Failure(status.error());
  }
  return Result<bool>::Success(resp.erased());
}

Result<RdmaCommitPartInfo> RdmaDataPlaneClient::CommitPart(
    const std::string& session_id, const std::string& upload_id,
    std::uint32_t part_number, std::uint64_t bytes_transferred,
    const std::string& client_crc32c_b64) const {
  if (!initialized()) {
    return Result<RdmaCommitPartInfo>::Failure(MakeNotInitialized("RDMA data plane client"));
  }
  brpc::Controller controller;
  ApplyRequestTimeout(controller, options_);
  us3_turbo_access::gateway::RdmaCommitPartRequest req;
  req.set_session_id(session_id);
  req.set_upload_id(upload_id);
  req.set_part_number(part_number);
  req.set_bytes_transferred(bytes_transferred);
  if (!client_crc32c_b64.empty()) {
    req.set_client_checksum(client_crc32c_b64);
  }
  us3_turbo_access::gateway::RdmaCommitPartResponse resp;
  stub_->CommitPart(&controller, &req, &resp, nullptr);
  auto status = CheckRpcFailure(controller, "CommitPart RPC failed",
                                DataFlow::CPUDirect, "");
  if (!status.success()) {
    return Result<RdmaCommitPartInfo>::Failure(status.error());
  }
  RdmaCommitPartInfo info;
  info.part_etag = resp.part_etag();
  return Result<RdmaCommitPartInfo>::Success(std::move(info));
}

}  // namespace us3_turbo_access::client
