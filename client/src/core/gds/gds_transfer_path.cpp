#include "client/src/core/gds/gds_transfer_path.h"

#include "client/src/core/common/errors.h"
#include "client/src/core/contracts/request_builder.h"

namespace us3_turbo_access::client {
namespace {

[[nodiscard]] Error MakeGdsUnsupportedError() {
  return MakeUnsupportedPath(DataPath::kGdsCuObject,
                             "The current environment does not support the GDS channel");
}

// 注册显存并向 gateway 协商出一个 TransferSession。
// memory_registry 验证 buffer 类型 / 非空，但 descriptor 字段已从 wire 协议
// 移除（gateway 用不到），所以这里只走校验。
template <typename BufferView>
[[nodiscard]] Result<TransferSession> OpenSession(const GdsContext& context, OperationType op,
                                                   const RequestOptions& request,
                                                   BufferView buffer) {
  auto registration = context.memory_registry.Register(op, buffer);
  if (!registration.success()) {
    return Result<TransferSession>::Failure(registration.error());
  }

  auto open_request = MakeSessionHandshake(context.options, SessionPlan{
      .operation = op,
      .request = request,
      .buffer_type = buffer.type,
      .path = DataPath::kGdsCuObject,
  });

  auto open_response = context.metadata_client.OpenTransferSession(open_request);
  if (!open_response.success()) {
    return Result<TransferSession>::Failure(open_response.error());
  }
  return Result<TransferSession>::Success(ImportSession(open_response.value()));
}

}  // namespace

GdsTransferPath::GdsTransferPath(const PlatformCapabilities& caps, const GdsContext& context)
    : caps_(caps), context_(context) {}

DataPath GdsTransferPath::path() const { return DataPath::kGdsCuObject; }

bool GdsTransferPath::available() const { return caps_.cuobject_available; }

Result<TransferOutcome> GdsTransferPath::GetObject(const RequestOptions& request,
                                                   MutableBufferView buffer) const {
  if (!available()) {
    return Result<TransferOutcome>::Failure(MakeGdsUnsupportedError());
  }

  auto session = OpenSession(context_, OperationType::kGet, request, buffer);
  if (!session.success()) {
    return Result<TransferOutcome>::Failure(session.error());
  }

  return context_.cuobj_client.ExecuteGet(context_.options, context_.data_client, session.value(),
                                          request, buffer);
}

Result<TransferOutcome> GdsTransferPath::PutObject(const RequestOptions& request,
                                                   ConstBufferView buffer) const {
  if (!available()) {
    return Result<TransferOutcome>::Failure(MakeGdsUnsupportedError());
  }

  auto session = OpenSession(context_, OperationType::kPut, request, buffer);
  if (!session.success()) {
    return Result<TransferOutcome>::Failure(session.error());
  }

  return context_.cuobj_client.ExecutePut(context_.options, context_.data_client, session.value(),
                                          request, buffer);
}

}  // namespace us3_turbo_access::client
