#include "client/src/core/upload/upload_coordinator.h"

#include "client/src/core/client/client_core.h"
#include "client/src/core/gds/gds_multipart_flow.h"
#include "client/src/core/http/http_multipart_flow.h"
#include "client/src/core/rdma/rdma_multipart_flow.h"

namespace us3_turbo_access::client {

UploadCoordinator::UploadCoordinator(ClientCore& core)
    : http_flow_(std::make_unique<HttpMultipartFlow>(
          core.http_data_client(), core.http_transfer_path())),
      rdma_flow_(std::make_unique<RdmaMultipartFlow>(
          core.metadata_client(), core.rdma_transfer_path())),
      gds_flow_(std::make_unique<GdsMultipartFlow>(
          core.metadata_client(), core.gds_transfer_path())) {}

IMultipartFlow& UploadCoordinator::SelectFlow(DataPath data_path) {
  switch (data_path) {
    case DataPath::kHttpTcp:    return *http_flow_;
    case DataPath::kNativeRdma: return *rdma_flow_;
    case DataPath::kGdsCuObject:
    default:                    return *gds_flow_;
  }
}

}  // namespace us3_turbo_access::client
