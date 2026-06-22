#include "client/src/core/routing/transfer_router.h"

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {
namespace {

[[nodiscard]] Error MakeNoAvailableTransportError(DataFlow path) {
  return MakeUnsupportedPath(path,
                             "The configured transfer channel is unavailable: " +
                                 std::string(ToString(path)));
}

}  // namespace

TransferRouter::TransferRouter(DataFlow data_flow, const TransferPath& gds_executor,
                               const TransferPath& rdma_executor)
    : data_path_(data_flow),
      gds_executor_(gds_executor),
      rdma_executor_(rdma_executor) {}

Result<TransferOutcome> TransferRouter::GetObject(const GetObjectRequest& request,
                                                  MutableBufferView buffer) const {
  const auto* executor = SelectTransferPath();
  if (executor == nullptr) {
    return Result<TransferOutcome>::Failure(MakeNoAvailableTransportError(data_path_));
  }
  return executor->GetObject(request, buffer);
}

Result<TransferOutcome> TransferRouter::PutObject(const PutObjectRequest& request,
                                                  ConstBufferView buffer) const {
  const auto* executor = SelectTransferPath();
  if (executor == nullptr || !executor->available()) {
    return Result<TransferOutcome>::Failure(MakeNoAvailableTransportError(data_path_));
  }
  return executor->PutObject(request, buffer);
}

const TransferPath* TransferRouter::SelectTransferPath() const {
  switch (data_path_) {
    case DataFlow::GPUDirect:
      return &gds_executor_;
    case DataFlow::CPUDirect:
      return &rdma_executor_;
  }
  return nullptr;
}

}  // namespace us3_turbo_access::client
