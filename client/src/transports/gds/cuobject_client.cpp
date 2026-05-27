#include "client/src/transports/gds/cuobject_client.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include <cufile.h>
#include <cuobjclient.h>

#include "client/src/data/gds_data_client.h"
#include "client/src/transports/gds/chunk_dispatcher.h"
#include "client/src/transports/gds/cuobj_library.h"
#include "client/src/core/common/errors.h"

namespace us3_turbo_access::client {
namespace {

// cuObjClient 实例的 RAII 容器：构造时 api.constructor 就地构造，
// 析构时 api.destructor 反向销毁。
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

  CuObjClientHandle(const CuObjClientHandle&) = delete;
  CuObjClientHandle& operator=(const CuObjClientHandle&) = delete;

  [[nodiscard]] void* raw() { return &storage_; }

 private:
  const CuObjApi& api_;
  bool constructed_{false};
  alignas(alignof(cuObjClient)) std::byte storage_[std::max(sizeof(cuObjClient), std::size_t{4096})];
};

// cuObj 描述符 RAII 守卫：cuMemObjGetDescriptor 注册的 GPU buffer
// 必须在传输结束后通过 cuMemObjPutDescriptor 释放，析构时自动调用。
class CuObjDescriptorGuard {
 public:
  CuObjDescriptorGuard(const CuObjApi& api, void* client_object,
                       void* registered_buffer)
      : api_(api),
        client_object_(client_object),
        registered_buffer_(registered_buffer) {}

  ~CuObjDescriptorGuard() {
    api_.put_descriptor(client_object_, registered_buffer_);
  }

  CuObjDescriptorGuard(const CuObjDescriptorGuard&) = delete;
  CuObjDescriptorGuard& operator=(const CuObjDescriptorGuard&) = delete;

 private:
  const CuObjApi& api_;
  void* client_object_;
  void* registered_buffer_;
};

struct CuObjCallbackContext {
  const ChunkDispatcher* dispatcher{nullptr};
  std::uint64_t base_offset{0};
  TransferOutcome outcome;
  std::optional<Error> callback_err;
};

/* ===== PoC: cuObj client / descriptor reuse probe =====
 * US3_GDS_POC_MODE env var (default 0 = unchanged original path):
 *   0 = global lock + per-call client + per-call descriptor (current production)
 *   1 = global lock + per-call client + cross-client cached descriptor
 *       (tests Q2: descriptor table truly process-wide?)
 *   2 = global lock + singleton client + cached descriptor on that client
 *   3 = no lock     + singleton client + cached descriptor
 *       (tests Q3: single client thread-safe under concurrent cuObjPut?)
 *   4 = no lock     + thread_local client + per-thread cached descriptor
 *       (tests Q4: N independent clients beat shared client?)
 * Descriptor never released for the lifetime of the cache; PoC only — no LRU. */
int GetPocMode() {
  static int m = []() {
    const char* e = std::getenv("US3_GDS_POC_MODE");
    return (e != nullptr && *e != '\0') ? std::atoi(e) : 0;
  }();
  return m;
}

// Forward decls of the cuObj callbacks used by ops table.
ssize_t CuObjGetCallback(const void*, char*, size_t, loff_t, const cufileRDMAInfo_t*);
ssize_t CuObjPutCallback(const void*, const char*, size_t, loff_t, const cufileRDMAInfo_t*);

// Long-lived cuObj client: owns its own ops table so the SDK reference stays
// valid for the lifetime of the client (PoC modes 2-4 keep clients alive).
class LongLivedCuObjClient {
 public:
  explicit LongLivedCuObjClient(const CuObjApi& api) {
    ops_.get = &CuObjGetCallback;
    ops_.put = &CuObjPutCallback;
    handle_ = std::make_unique<CuObjClientHandle>(api, ops_);
  }
  [[nodiscard]] void* raw() { return handle_->raw(); }

 private:
  CUObjOps_t ops_{};
  std::unique_ptr<CuObjClientHandle> handle_;
};

// Mode 1: process-wide cross-client descriptor set.
std::mutex                g_xclient_reg_mu;
std::unordered_set<void*> g_xclient_registered;

// Modes 2/3: singleton client + its descriptor set.
std::mutex                          g_shared_init_mu;
std::unique_ptr<LongLivedCuObjClient> g_shared_client;
std::mutex                          g_shared_reg_mu;
std::unordered_set<void*>           g_shared_registered;

LongLivedCuObjClient& GetSharedClient(const CuObjApi& api) {
  std::scoped_lock lk(g_shared_init_mu);
  if (!g_shared_client) {
    g_shared_client = std::make_unique<LongLivedCuObjClient>(api);
  }
  return *g_shared_client;
}

// Mode 4: thread-local client + per-thread descriptor set.
struct ThreadLocalState {
  std::unique_ptr<LongLivedCuObjClient> client;
  std::unordered_set<void*>             registered;
};
ThreadLocalState& GetTlState(const CuObjApi& api) {
  thread_local ThreadLocalState s;
  if (!s.client) {
    s.client = std::make_unique<LongLivedCuObjClient>(api);
  }
  return s;
}

/* Ensure descriptor for `ptr` is registered; hold the lock through the SDK
 * register call so first-time concurrent callers serialize on this single
 * register, not race past the lookup. After the first successful register
 * subsequent callers short-circuit on the set lookup. */
[[nodiscard]] cuObjErr_t EnsureRegistered(const CuObjApi& api, void* client_raw,
                                          void* ptr, std::size_t size,
                                          std::mutex& mu,
                                          std::unordered_set<void*>& seen) {
  std::scoped_lock lk(mu);
  if (seen.find(ptr) != seen.end()) return CU_OBJ_SUCCESS;
  const auto rc = api.get_descriptor(client_raw, ptr, size);
  if (rc == CU_OBJ_SUCCESS) seen.insert(ptr);
  return rc;
}
/* ===== /PoC ===== */

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

/**
 * 单次 GDS 数据面传输的总驱动。
 * 装配 cuObjClient → 注册 GPU buffer → 触发 cuObj GET/PUT，
 * cuObj 内部按 chunk 回调 ChunkDispatcher 发 GdsGet/GdsPut RPC。
 * upload_id 非空时切换为 multipart 模式，chunk 走 PutPart 而非 PutChunk。
 */
template <typename BufferPointer>
[[nodiscard]] Result<TransferOutcome> ExecuteTransfer(const ClientOptions& options,
                                                      const GdsDataClient& data_client,
                                                      const TransferSession& session,
                                                      const RequestOptions& request,
                                                      BufferPointer buffer,
                                                      std::size_t buffer_size,
                                                      OperationType op,
                                                      const std::string& upload_id = {},
                                                      std::uint32_t part_number = 0) {
  CuObjCallbackContext context;
  try {
    // 1. 加载 cuObjClient 动态库并解析符号
    auto library = CuObjLibrary::Get();
    if (!library.success()) {
      return Result<TransferOutcome>::Failure(library.error());
    }
    const auto& api = library.value()->api();
    auto* mutable_buffer = const_cast<void*>(static_cast<const void*>(buffer));

    // 2. 装配 cuObj 回调函数表（GET / PUT 用同一份 dispatcher）
    CUObjOps_t ops{};
    ops.get = &CuObjGetCallback;
    ops.put = &CuObjPutCallback;

    /* 3-5. 客户端获取 + descriptor 注册：按 US3_GDS_POC_MODE 切换实现。
     *      mode 0 保留原行为；mode 1-4 是 PoC 路径，descriptor 缓存不释放。 */
    static std::mutex g_cuobj_global_mu;
    const int poc_mode = GetPocMode();
    std::optional<std::scoped_lock<std::mutex>> global_lock_holder;
    std::optional<CuObjClientHandle> per_call_client;
    std::optional<CuObjDescriptorGuard> per_call_desc_guard;
    void* client_raw = nullptr;

    auto fail_register = [&]() {
      return Result<TransferOutcome>::Failure(
          MakeCuObjError(session.meta.request_id,
                         "cuMemObjGetDescriptor 注册失败", true));
    };
    auto fail_connect = [&]() {
      return Result<TransferOutcome>::Failure(
          MakeCuObjError(session.meta.request_id,
                         "cuObjClient 未连接到可用的 RDMA 服务", true));
    };

    if (poc_mode == 0) {
      global_lock_holder.emplace(g_cuobj_global_mu);
      per_call_client.emplace(api, ops);
      client_raw = per_call_client->raw();
      if (!api.is_connected(client_raw)) return fail_connect();
      if (api.get_descriptor(client_raw, mutable_buffer, buffer_size) != CU_OBJ_SUCCESS)
        return fail_register();
      per_call_desc_guard.emplace(api, client_raw, mutable_buffer);
    } else if (poc_mode == 1) {
      global_lock_holder.emplace(g_cuobj_global_mu);
      per_call_client.emplace(api, ops);
      client_raw = per_call_client->raw();
      if (!api.is_connected(client_raw)) return fail_connect();
      if (EnsureRegistered(api, client_raw, mutable_buffer, buffer_size,
                            g_xclient_reg_mu, g_xclient_registered) != CU_OBJ_SUCCESS)
        return fail_register();
      // no guard: descriptor lives for process lifetime (PoC)
    } else if (poc_mode == 2 || poc_mode == 3) {
      if (poc_mode == 2) global_lock_holder.emplace(g_cuobj_global_mu);
      auto& shared = GetSharedClient(api);
      client_raw = shared.raw();
      if (!api.is_connected(client_raw)) return fail_connect();
      if (EnsureRegistered(api, client_raw, mutable_buffer, buffer_size,
                            g_shared_reg_mu, g_shared_registered) != CU_OBJ_SUCCESS)
        return fail_register();
    } else /* poc_mode == 4 */ {
      auto& tl = GetTlState(api);
      client_raw = tl.client->raw();
      if (!api.is_connected(client_raw)) return fail_connect();
      /* Descriptor table is process-wide (verified by mode 1). Reuse the
       * cross-client set so different per-thread clients don't double-register
       * the same ptr. */
      if (EnsureRegistered(api, client_raw, mutable_buffer, buffer_size,
                            g_xclient_reg_mu, g_xclient_registered) != CU_OBJ_SUCCESS)
        return fail_register();
    }

    /* 6. 装配 ChunkDispatcher：每次 cuObj chunk 回调对应一条 RPC。
     *    upload_id 非空时切到 multipart 模式：chunk_offset 按 part 内偏移上报，
     *    并带上 upload_id + part_number，gateway 据此走 WritePart 而非 WriteRange。 */
    ChunkDispatcher dispatcher(options, data_client, session, request, op);
    if (!upload_id.empty()) {
      dispatcher.SetMultipart(upload_id, part_number);
    }

    /* 7. 初始化 callback 上下文。
     *    context 必须在整个 cuObjGet/cuObjPut 调用期内保持有效，
     *    cuObj 回调线程会持续读写其内的 dispatcher / outcome。 */
    context.dispatcher = &dispatcher;
    context.base_offset = request.offset;
    context.outcome.selected_path = DataPath::kGdsCuObject;
    context.outcome.request_id = session.meta.request_id;
    context.outcome.session_id = session.meta.session_id;
    context.outcome.gateway_id = session.meta.gateway_id;

    // 8. 校验请求字节数（取 request.length 与 buffer_size 的较小值）
    const auto req_bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(request.length.value_or(buffer_size), buffer_size));
    if (req_bytes == 0U) {
      return Result<TransferOutcome>::Failure(MakeCuObjError(
          session.meta.request_id,
          "请求字节数为零，无法发起 GDS 传输", false));
    }

    // 9. 同步驱动 cuObj 传输；返回前 dispatcher 已经把所有 chunk RPC 发完
    const ssize_t result = op == OperationType::kGet
                               ? api.cuobj_get(client_raw, &context, mutable_buffer, req_bytes, 0, 0)
                               : api.cuobj_put(client_raw, &context, mutable_buffer, req_bytes, 0, 0);

    /* 10. 错误判定：callback 内部失败优先（带 RPC 层错误信息），
     *     其次才看 cuObj 自身返回码。 */
    if (context.callback_err.has_value()) {
      return Result<TransferOutcome>::Failure(*context.callback_err);
    }
    if (result < 0) {
      return Result<TransferOutcome>::Failure(
          MakeCuObjError(session.meta.request_id,
                         op == OperationType::kGet ? "cuObjGet 执行失败"
                                                   : "cuObjPut 执行失败",
                         true));
    }

    // 11. 汇总传输结果，回调可选的 progress notifier
    context.outcome.bytes_transferred = static_cast<std::size_t>(result);
    if (context.outcome.transfer_status.empty()) {
      context.outcome.transfer_status = "completed";
    }
    if (request.progress_callback) {
      request.progress_callback({.bytes_completed = context.outcome.bytes_transferred,
                                 .bytes_total = context.outcome.bytes_transferred,
                                 .data_path = DataPath::kGdsCuObject});
    }

    // 12. 返回成功 outcome
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

Result<TransferOutcome> CuObjectClient::ExecutePutPart(
    const ClientOptions& options, const GdsDataClient& data_client,
    const TransferSession& session, const RequestOptions& request,
    ConstBufferView buffer, const std::string& upload_id,
    std::uint32_t part_number) const {
  return ExecuteTransfer(options, data_client, session, request, buffer.data,
                         buffer.size, OperationType::kPut, upload_id, part_number);
}

}  // namespace us3_turbo_access::client
