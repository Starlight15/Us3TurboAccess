#include "client/src/transports/gds/cuobject_client.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include <cufile.h>
#include <cuobjclient.h>

#include "client/src/data/gds_data_client.h"
#include "client/src/transports/gds/chunk_dispatcher.h"
#include "client/src/transports/gds/cuobj_library.h"
#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {
namespace {

class CuObjClientHandle {
 public:
  CuObjClientHandle(const CuObjApi& api, CUObjOps_t ops) : api_(api) {
    api_.constructor(&storage_, ops, CUOBJ_PROTO_RDMA_DC_V1);
    constructed_ = true;
  }

  ~CuObjClientHandle() {
    if (constructed_) {
      api_.destructor(&storage_);
    }
  }

  [[nodiscard]] void* raw() { return &storage_; }

 private:
  const CuObjApi& api_;
  bool constructed_{false};
  alignas(alignof(cuObjClient)) std::byte storage_[std::max(sizeof(cuObjClient), std::size_t{4096})];
};

struct CuObjCallbackContext {
  const ChunkDispatcher* dispatcher{nullptr};
  std::uint64_t base_offset{0};
  TransferOutcome outcome;
  std::optional<Error> callback_err;
};

[[nodiscard]] Error MakeCuObjError(const std::string& request_id, const std::string& message,
                                   bool retryable) {
  return MakeTransportFailure(message, DataPath::kGdsCuObject, request_id, retryable);
}

[[nodiscard]] std::string RdmaTokenFromInfo(const cufileRDMAInfo_t* rdma_info) {
  if (rdma_info == nullptr || rdma_info->desc_str == nullptr) {
    return {};
  }
  std::size_t len = rdma_info->desc_len > 0 ? static_cast<std::size_t>(rdma_info->desc_len)
                                            : std::strlen(rdma_info->desc_str);
  while (len > 0 && rdma_info->desc_str[len - 1] == '\0') {
    --len;
  }
  return std::string(rdma_info->desc_str, rdma_info->desc_str + len);
}

[[nodiscard]] CuObjCallbackContext* ResolveCallbackContext(const void* handle,
                                                           const cufileRDMAInfo_t* rdma_info) {
  static_cast<void>(rdma_info);
  if (handle == nullptr) {
    return nullptr;
  }
  auto lib = CuObjLibrary::Get();
  if (!lib.success()) {
    return nullptr;
  }
  return static_cast<CuObjCallbackContext*>(lib.value()->api().get_ctx(handle));
}

[[nodiscard]] ssize_t ExecuteControlCallback(CuObjCallbackContext* context, std::size_t size,
                                             loff_t offset, const cufileRDMAInfo_t* rdma_info) {
  const std::string rdma_token = RdmaTokenFromInfo(rdma_info);
  auto result = context->dispatcher->Dispatch(rdma_token,
                                               context->base_offset + static_cast<std::uint64_t>(offset),
                                               size);
  if (!result.success()) {
    context->callback_err = result.error();
    return -1;
  }

  const auto& outcome = result.value();
  if (!outcome.gateway_id.empty()) context->outcome.gateway_id = outcome.gateway_id;
  if (!outcome.transfer_status.empty()) context->outcome.transfer_status = outcome.transfer_status;
  if (!outcome.rdma_reply.empty()) context->outcome.rdma_reply = outcome.rdma_reply;
  if (!outcome.etag.empty()) context->outcome.etag = outcome.etag;
  if (!outcome.version.empty()) context->outcome.version = outcome.version;
  context->outcome.bytes_transferred += size;
  return static_cast<ssize_t>(size);
}

ssize_t CuObjGetCallback(const void* handle, char* ptr, size_t size, loff_t offset,
                         const cufileRDMAInfo_t* rdma_info) {
  static_cast<void>(ptr);
  auto* context = ResolveCallbackContext(handle, rdma_info);
  if (context == nullptr) {
    return -1;
  }
  return ExecuteControlCallback(context, size, offset, rdma_info);
}

ssize_t CuObjPutCallback(const void* handle, const char* ptr, size_t size, loff_t offset,
                         const cufileRDMAInfo_t* rdma_info) {
  static_cast<void>(ptr);
  auto* context = ResolveCallbackContext(handle, rdma_info);
  if (context == nullptr) {
    return -1;
  }
  return ExecuteControlCallback(context, size, offset, rdma_info);
}

template <typename BufferPointer>
[[nodiscard]] Result<TransferOutcome> ExecuteTransfer(const ClientOptions& options,
                                                      const GdsDataClient& data_client,
                                                      const TransferSession& session,
                                                      const RequestOptions& request,
                                                      BufferPointer buffer,
                                                      std::size_t buffer_size,
                                                      OperationType op) {
  CuObjCallbackContext context;
  try {
    auto library = CuObjLibrary::Get();
    if (!library.success()) {
      return Result<TransferOutcome>::Failure(library.error());
    }
    const auto& api = library.value()->api();
    auto* mutable_buffer = const_cast<void*>(static_cast<const void*>(buffer));

    CUObjOps_t ops{};
    ops.get = &CuObjGetCallback;
    ops.put = &CuObjPutCallback;

    CuObjClientHandle client(api, ops);
    if (!api.is_connected(client.raw())) {
      return Result<TransferOutcome>::Failure(
          MakeCuObjError(session.meta.request_id, "cuObjClient is not connected to an available RDMA service", true));
    }
    if (api.get_descriptor(client.raw(), mutable_buffer, buffer_size) != CU_OBJ_SUCCESS) {
      return Result<TransferOutcome>::Failure(
          MakeCuObjError(session.meta.request_id, "cuMemObjGetDescriptor registration failed", true));
    }

    struct DescriptorGuard {
      DescriptorGuard(const CuObjApi& api, void* client_object, void* registered_buffer)
          : api(api), client_object(client_object), registered_buffer(registered_buffer) {}
      ~DescriptorGuard() { api.put_descriptor(client_object, registered_buffer); }
      const CuObjApi& api;
      void* client_object;
      void* registered_buffer;
    } descriptor_guard(api, client.raw(), mutable_buffer);

    ChunkDispatcher dispatcher(options, data_client, session, request, op);
    context.dispatcher = &dispatcher;
    context.base_offset = request.offset;
    context.outcome.selected_path = DataPath::kGdsCuObject;
    context.outcome.request_id = session.meta.request_id;
    context.outcome.session_id = session.meta.session_id;
    context.outcome.gateway_id = session.meta.gateway_id;

    // context 必须在整个 cuObjGet/cuObjPut 调用期内保持有效，callback 持续读写其内容。
    const auto req_bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(request.length.value_or(buffer_size), buffer_size));
    if (req_bytes == 0U) {
      return Result<TransferOutcome>::Failure(MakeCuObjError(
          session.meta.request_id, "requested_bytes is zero; cannot execute a GDS transfer", false));
    }

    const ssize_t result = op == OperationType::kGet
                               ? api.cuobj_get(client.raw(), &context, mutable_buffer, req_bytes, 0, 0)
                               : api.cuobj_put(client.raw(), &context, mutable_buffer, req_bytes, 0, 0);

    if (context.callback_err.has_value()) {
      return Result<TransferOutcome>::Failure(*context.callback_err);
    }
    if (result < 0) {
      return Result<TransferOutcome>::Failure(
          MakeCuObjError(session.meta.request_id,
                         op == OperationType::kGet ? "cuObjGet execution failed" : "cuObjPut execution failed",
                         true));
    }

    context.outcome.bytes_transferred = static_cast<std::size_t>(result);
    if (context.outcome.transfer_status.empty()) {
      context.outcome.transfer_status = "completed";
    }
    if (request.progress_callback) {
      request.progress_callback({.bytes_completed = context.outcome.bytes_transferred,
                                 .bytes_total = context.outcome.bytes_transferred,
                                 .data_path = DataPath::kGdsCuObject});
    }
    return Result<TransferOutcome>::Success(std::move(context.outcome));
  } catch (const std::bad_alloc& e) {
    if (context.callback_err.has_value()) {
      return Result<TransferOutcome>::Failure(*context.callback_err);
    }
    return Result<TransferOutcome>::Failure(MakeCuObjError(
        session.meta.request_id, "cuObjClient memory allocation failed (bad_alloc): " + std::string(e.what()),
        true));
  } catch (const std::exception& e) {
    return Result<TransferOutcome>::Failure(
        MakeCuObjError(session.meta.request_id, "cuObjClient exception: " + std::string(e.what()), true));
  } catch (...) {
    return Result<TransferOutcome>::Failure(
        MakeCuObjError(session.meta.request_id, "cuObjClient unknown exception", true));
  }
}

}  // namespace

Result<TransferOutcome> CuObjectClient::ExecuteGet(const ClientOptions& options,
                                                   const GdsDataClient& data_client,
                                                   const TransferSession& session,
                                                   const RequestOptions& request,
                                                   MutableBufferView buffer) const {
  return ExecuteTransfer(options, data_client, session, request, buffer.data, buffer.size,
                         OperationType::kGet);
}

Result<TransferOutcome> CuObjectClient::ExecutePut(const ClientOptions& options,
                                                   const GdsDataClient& data_client,
                                                   const TransferSession& session,
                                                   const RequestOptions& request,
                                                   ConstBufferView buffer) const {
  return ExecuteTransfer(options, data_client, session, request, buffer.data, buffer.size,
                         OperationType::kPut);
}

}  // namespace us3_turbo_access::client
