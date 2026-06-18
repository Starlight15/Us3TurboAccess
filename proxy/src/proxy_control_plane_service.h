#pragma once

#include <string>

#include <brpc/controller.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "control_plane.pb.h"
#include "proxy/src/session/session_manager.h"

namespace us3_turbo_access::proxy {

/**
 * @brief 无状态控制面服务：仅实现 OpenSession（GDS 单对象 PUT），
 *        其余 7 个 RPC 一律返回 "not implemented in proxy v1"。
 *
 * v1 仅做可行性验证——发 session + ticket，不做 Reserve、不持索引、
 * 不连 cuObjServer。session 生命周期/索引留待后续。
 *
 * session 凭证生成委托给 SessionManager（引用，非拥有）。
 */
class ProxyControlPlaneService final
    : public ::us3_turbo_access::gateway::ControlPlaneService {
 public:
  ProxyControlPlaneService(std::string gateway_id,
                           SessionManager& session_mgr);

  // ---- 唯一真实实现的 RPC ----
  void OpenSession(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo_access::gateway::OpenSessionRequest* request,
      ::us3_turbo_access::gateway::OpenSessionResponse* response,
      google::protobuf::Closure* done) override;

  // ---- v1 占位：不实现 ----
  void HeadObject(google::protobuf::RpcController* cntl,
                  const ::us3_turbo_access::gateway::HeadObjectRequest* request,
                  ::us3_turbo_access::gateway::HeadObjectResponse* response,
                  google::protobuf::Closure* done) override;

  void GdsGet(google::protobuf::RpcController* cntl,
              const ::us3_turbo_access::gateway::GdsChunkRequest* request,
              ::us3_turbo_access::gateway::GdsChunkResponse* response,
              google::protobuf::Closure* done) override;

  void GdsPut(google::protobuf::RpcController* cntl,
              const ::us3_turbo_access::gateway::GdsChunkRequest* request,
              ::us3_turbo_access::gateway::GdsChunkResponse* response,
              google::protobuf::Closure* done) override;

  void AbortSession(google::protobuf::RpcController* cntl,
                    const ::us3_turbo_access::gateway::AbortSessionRequest* request,
                    ::us3_turbo_access::gateway::AbortSessionResponse* response,
                    google::protobuf::Closure* done) override;

  void StartUpload(google::protobuf::RpcController* cntl,
                   const ::us3_turbo_access::gateway::StartUploadRequest* request,
                   ::us3_turbo_access::gateway::StartUploadResponse* response,
                   google::protobuf::Closure* done) override;

  void CompleteUpload(google::protobuf::RpcController* cntl,
                      const ::us3_turbo_access::gateway::CompleteUploadRequest* request,
                      ::us3_turbo_access::gateway::CompleteUploadResponse* response,
                      google::protobuf::Closure* done) override;

  void AbortUpload(google::protobuf::RpcController* cntl,
                   const ::us3_turbo_access::gateway::AbortUploadRequest* request,
                   ::us3_turbo_access::gateway::AbortUploadResponse* response,
                   google::protobuf::Closure* done) override;

 private:
  std::string gateway_id_;
  SessionManager& session_mgr_;
};

}  // namespace us3_turbo_access::proxy
