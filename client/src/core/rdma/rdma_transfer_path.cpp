#include "client/src/core/rdma/rdma_transfer_path.h"

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {
namespace {

[[nodiscard]] Error MakeNotImplementedError() {
  return MakeUnsupportedPath(DataPath::kNativeRdma,
                             "The native RDMA channel is not implemented yet");
}

}  // namespace

DataPath RdmaTransferPath::path() const { return DataPath::kNativeRdma; }

bool RdmaTransferPath::available() const { return false; }

Result<TransferOutcome> RdmaTransferPath::GetObject(const RequestOptions&,
                                                    MutableBufferView) const {
  return Result<TransferOutcome>::Failure(MakeNotImplementedError());
}

Result<TransferOutcome> RdmaTransferPath::PutObject(const RequestOptions&, ConstBufferView) const {
  return Result<TransferOutcome>::Failure(MakeNotImplementedError());
}

}  // namespace us3_turbo_access::client
