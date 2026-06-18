#pragma once

#include <string>

#include <brpc/controller.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>

#include "backend/src/backend_gds_sink.h"
#include "control_plane.pb.h"

namespace us3_turbo_access::backend {

/**
 * @brief 数据面服务：仅真实实现 GdsPut 的单对象分支（upload_id 为空），
 *        用客户端 token 反向 RDMA-READ 拉字节后丢弃。
 *
 * 其余 7 个 RPC 与“GdsPut 收到非空 upload_id”一律
 * cntl->SetFailed("backend v1: only single-object GdsPut")。
 * v1 不校验 session/ticket（proxy 是另一进程）。
 */
class BackendDataPlaneService final
    : public ::us3_turbo_access::gateway::ControlPlaneService {
 public:
  BackendDataPlaneService(BackendGdsSink& sink, std::string gateway_id);

  void OpenSession(
      google::protobuf::RpcController* cntl,
      const ::us3_turbo_access::gateway::OpenSessionRequest* request,
      ::us3_turbo_access::gateway::OpenSessionResponse* response,
      google::protobuf::Closure* done) override;

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
  BackendGdsSink& sink_;
  std::string     gateway_id_;
};

}  // namespace us3_turbo_access::backend
