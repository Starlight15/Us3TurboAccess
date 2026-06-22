#include "client/src/core/upload/upload_coordinator.h"

#include "client/src/core/client/client_core.h"
#include "client/src/core/gds/gds_multipart_flow.h"
#include "client/src/core/rdma/rdma_multipart_flow.h"

namespace us3_turbo_access::client {

UploadCoordinator::UploadCoordinator(ClientCore& core)
    : rdma_flow_(std::make_unique<RdmaMultipartFlow>(
          core.metadata_client(), core.rdma_transfer_path())),
      gds_flow_(std::make_unique<GdsMultipartFlow>(
          core.metadata_client(), core.gds_transfer_path())) {}

IMultipartFlow& UploadCoordinator::SelectFlow(DataFlow data_flow) {
  switch (data_flow) {
    case DataFlow::CPUDirect:  return *rdma_flow_;
    case DataFlow::GPUDirect:
    default:                     return *gds_flow_;
  }
}

}  // namespace us3_turbo_access::client
