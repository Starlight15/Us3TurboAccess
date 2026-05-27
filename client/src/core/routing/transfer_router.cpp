#include "client/src/core/routing/transfer_router.h"

#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {
namespace {

[[nodiscard]] Error MakeNoAvailableTransportError(DataPath path) {
  return MakeUnsupportedPath(path,
                             "The configured transfer channel is unavailable: " +
                                 std::string(ToString(path)));
}

}  // namespace

TransferRouter::TransferRouter(DataPath data_path, const TransferPath& gds_executor,
                               const TransferPath& rdma_executor,
                               const TransferPath& http_executor)
    : data_path_(data_path),
      gds_executor_(gds_executor),
      rdma_executor_(rdma_executor),
      http_executor_(http_executor) {}

Result<TransferOutcome> TransferRouter::GetObject(const RequestOptions& request,
                                                  MutableBufferView buffer) const {
  const auto* executor = SelectTransferPath();
  if (executor == nullptr || !executor->available()) {
    return Result<TransferOutcome>::Failure(MakeNoAvailableTransportError(data_path_));
  }
  return executor->GetObject(request, buffer);
}

Result<TransferOutcome> TransferRouter::PutObject(const RequestOptions& request,
                                                  ConstBufferView buffer) const {
  const auto* executor = SelectTransferPath();
  if (executor == nullptr || !executor->available()) {
    return Result<TransferOutcome>::Failure(MakeNoAvailableTransportError(data_path_));
  }
  return executor->PutObject(request, buffer);
}

const TransferPath* TransferRouter::SelectTransferPath() const {
  switch (data_path_) {
    case DataPath::kGdsCuObject:
      return &gds_executor_;
    case DataPath::kNativeRdma:
      return &rdma_executor_;
    case DataPath::kHttpTcp:
      return &http_executor_;
  }
  return nullptr;
}

}  // namespace us3_turbo_access::client
