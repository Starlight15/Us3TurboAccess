#include "client/src/transports/gds/gds_memory_registry.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>

#include <cufile.h>
#include <cuobjclient.h>

#include "client/src/core/common/errors.h"
#include "client/src/transports/gds/cuobj_library.h"

namespace us3_turbo_access::client {
namespace {

/* ===== 进程级单例：cuObj client + descriptor 缓存 =====
 *
 * 进程内只活一份 cuObjClient（PoC 验证 GetRDMAToken 多线程安全），所有
 * Client 实例共享。SDK 的 descriptor 表本来也是 process-wide。
 *
 * - 注册路径（RegisterBuffer / UnregisterBuffer）短锁串行；不在数据面热路径
 * - AcquireToken 数据路径上无锁；GetRDMAToken 自身线程安全 */

// cuObj PUT/GET 回调在 token-direct 路径下永远不会触发。SDK 构造 client
// 时要求 ops 表两个字段非空；给一个返 -1 的桩。
ssize_t StubGet(const void*, char*, size_t, loff_t, const cufileRDMAInfo_t*) {
  return -1;
}
ssize_t StubPut(const void*, const char*, size_t, loff_t, const cufileRDMAInfo_t*) {
  return -1;
}

class CuObjState {
 public:
  // 返回进程唯一的状态实例；首次访问触发 cuObj 库加载 + client 构造。
  static Result<CuObjState*> Get() {
    static Result<CuObjState*> holder = []() -> Result<CuObjState*> {
      auto lib = CuObjLibrary::Get();
      if (!lib.success()) {
        return Result<CuObjState*>::Failure(lib.error());
      }
      static CuObjState st(lib.value()->api());
      if (!st.connected_) {
        return Result<CuObjState*>::Failure(MakeTransportFailure(
            "cuObjClient 未连接到可用的 RDMA 服务", DataPath::kGdsCuObject, "",
            true));
      }
      return Result<CuObjState*>::Success(&st);
    }();
    return holder;
  }

  [[nodiscard]] const CuObjApi& api() const { return api_; }
  [[nodiscard]] void*           client_raw() { return &client_storage_; }

  // 内部状态：descriptor 表
  std::mutex                            registration_mu_;
  std::unordered_map<void*, std::size_t> registered_;

  ~CuObjState() {
    /* 进程退出时尽力反注册尚未释放的 descriptor，避免下次 cudaFree 时
     * nvidia-fs 死锁。这是 best-effort，正确做法仍是用户显式
     * UnregisterDeviceBuffer。 */
    if (!registered_.empty()) {
      std::cerr << "[gds_memory_registry] WARNING: " << registered_.size()
                << " device buffer(s) not unregistered before shutdown; "
                   "calling cuMemObjPutDescriptor as best-effort"
                << std::endl;
      for (auto& [ptr, _] : registered_) {
        api_.put_descriptor(&client_storage_, ptr);
      }
      registered_.clear();
    }
    if (constructed_) {
      api_.destructor(&client_storage_);
    }
  }

 private:
  explicit CuObjState(const CuObjApi& api) : api_(api) {
    ops_.get = &StubGet;
    ops_.put = &StubPut;
    api_.constructor(&client_storage_, ops_, CUOBJ_PROTO_RDMA_DC_V1);
    constructed_ = true;
    connected_   = api_.is_connected(&client_storage_);
    if (!connected_) {
      api_.destructor(&client_storage_);
      constructed_ = false;
    }
  }

  CuObjApi   api_;
  CUObjOps_t ops_{};
  bool       constructed_{false};
  bool       connected_{false};
  alignas(alignof(cuObjClient)) std::byte
      client_storage_[std::max(sizeof(cuObjClient), std::size_t{4096})];
};

[[nodiscard]] Result<bool> RegisterUnderLock(CuObjState& st, void* ptr,
                                              std::size_t size) {
  // 调用方持有 st.registration_mu_。
  auto it = st.registered_.find(ptr);
  if (it != st.registered_.end()) {
    // 已注册。若 size 不一致仍 success（descriptor 是地址级缓存，size 已
    // 在 SDK 内部记下）；这里只是 sanity 提示。
    return Result<bool>::Success(true);
  }
  const auto rc = st.api().get_descriptor(st.client_raw(), ptr, size);
  if (rc != CU_OBJ_SUCCESS) {
    return Result<bool>::Failure(MakeTransportFailure(
        "cuMemObjGetDescriptor 注册失败", DataPath::kGdsCuObject, "", true));
  }
  st.registered_.emplace(ptr, size);
  return Result<bool>::Success(true);
}

template <typename Pointer>
[[nodiscard]] Result<RegisteredBuffer> ValidateBuffer(Pointer data,
                                                      std::size_t size,
                                                      BufferType type) {
  if (type != BufferType::kCudaDevice) {
    return Result<RegisteredBuffer>::Failure(MakeUnsupportedPath(
        DataPath::kGdsCuObject,
        "The GDS channel only supports CUDA device buffers"));
  }
  if (data == nullptr || size == 0U) {
    return Result<RegisteredBuffer>::Failure(MakeInvalidArgument(
        "GDS buffers must be non-null and have a positive size"));
  }
  return Result<RegisteredBuffer>::Success(RegisteredBuffer{});
}

}  // namespace

/* ===== GdsRdmaToken ===== */

GdsRdmaToken::GdsRdmaToken(GdsRdmaToken&& other) noexcept
    : api_(other.api_), client_(other.client_), tok_(other.tok_) {
  other.api_    = nullptr;
  other.client_ = nullptr;
  other.tok_    = nullptr;
}

GdsRdmaToken& GdsRdmaToken::operator=(GdsRdmaToken&& other) noexcept {
  if (this != &other) {
    Reset();
    api_          = other.api_;
    client_       = other.client_;
    tok_          = other.tok_;
    other.api_    = nullptr;
    other.client_ = nullptr;
    other.tok_    = nullptr;
  }
  return *this;
}

GdsRdmaToken::~GdsRdmaToken() { Reset(); }

void GdsRdmaToken::Reset() noexcept {
  if (tok_ != nullptr && api_ != nullptr) {
    api_->put_rdma_token(client_, tok_);
  }
  api_    = nullptr;
  client_ = nullptr;
  tok_    = nullptr;
}

std::string_view GdsRdmaToken::str() const noexcept {
  if (tok_ == nullptr) return {};
  return std::string_view(tok_);
}

/* ===== GdsMemoryRegistry ===== */

Result<RegisteredBuffer>
GdsMemoryRegistry::Register(OperationType, MutableBufferView buffer) const {
  return ValidateBuffer(buffer.data, buffer.size, buffer.type);
}

Result<RegisteredBuffer>
GdsMemoryRegistry::Register(OperationType, ConstBufferView buffer) const {
  return ValidateBuffer(buffer.data, buffer.size, buffer.type);
}

Result<bool> GdsMemoryRegistry::RegisterBuffer(void* ptr, std::size_t size) {
  if (ptr == nullptr || size == 0U) {
    return Result<bool>::Failure(MakeInvalidArgument(
        "RegisterDeviceBuffer requires non-null ptr and positive size"));
  }
  auto state = CuObjState::Get();
  if (!state.success()) return Result<bool>::Failure(state.error());
  std::scoped_lock lk(state.value()->registration_mu_);
  return RegisterUnderLock(*state.value(), ptr, size);
}

Result<bool> GdsMemoryRegistry::UnregisterBuffer(void* ptr) {
  if (ptr == nullptr) {
    return Result<bool>::Failure(
        MakeInvalidArgument("UnregisterDeviceBuffer requires non-null ptr"));
  }
  auto state = CuObjState::Get();
  if (!state.success()) return Result<bool>::Failure(state.error());
  auto& st = *state.value();
  std::scoped_lock lk(st.registration_mu_);
  auto it = st.registered_.find(ptr);
  if (it == st.registered_.end()) {
    // 双反注册或者从未注册过；保持 idempotent 不报错。
    return Result<bool>::Success(true);
  }
  const auto rc = st.api().put_descriptor(st.client_raw(), ptr);
  st.registered_.erase(it);
  if (rc != CU_OBJ_SUCCESS) {
    return Result<bool>::Failure(MakeTransportFailure(
        "cuMemObjPutDescriptor 释放失败", DataPath::kGdsCuObject, "", true));
  }
  return Result<bool>::Success(true);
}

Result<GdsRdmaToken>
GdsMemoryRegistry::AcquireToken(void* ptr, std::size_t size,
                                 std::size_t offset, OperationType op) {
  if (ptr == nullptr || size == 0U) {
    return Result<GdsRdmaToken>::Failure(MakeInvalidArgument(
        "AcquireToken requires non-null ptr and positive size"));
  }
  auto state = CuObjState::Get();
  if (!state.success()) return Result<GdsRdmaToken>::Failure(state.error());
  auto& st = *state.value();

  /* 数据路径不持锁：注册路径已经在 RegisterBuffer 时完成；这里只读
   * descriptor 表确认存在，未存在则 lazy register（持锁）。 */
  bool need_register = false;
  {
    std::scoped_lock lk(st.registration_mu_);
    need_register = st.registered_.find(ptr) == st.registered_.end();
  }
  if (need_register) {
    std::scoped_lock lk(st.registration_mu_);
    auto reg = RegisterUnderLock(st, ptr, size + offset);
    if (!reg.success()) return Result<GdsRdmaToken>::Failure(reg.error());
  }

  // GetRDMAToken 在共享 client 上的多线程并发已 PoC 验证安全。
  const auto cu_op = op == OperationType::kGet ? CUOBJ_GET : CUOBJ_PUT;
  char* tok = nullptr;
  const auto rc =
      st.api().get_rdma_token(st.client_raw(), ptr, size, offset, cu_op, &tok);
  if (rc != CU_OBJ_SUCCESS || tok == nullptr) {
    return Result<GdsRdmaToken>::Failure(MakeTransportFailure(
        "cuMemObjGetRDMAToken 获取失败", DataPath::kGdsCuObject, "", true));
  }
  return Result<GdsRdmaToken>::Success(
      GdsRdmaToken(&st.api(), st.client_raw(), tok));
}

Result<GdsRdmaToken>
GdsMemoryRegistry::AcquireToken(const void* ptr, std::size_t size,
                                 std::size_t offset, OperationType op) {
  // 内部 const_cast 一次：cuMemObjGetRDMAToken 签名是 CUdeviceptr
  // （本质 uint64_t），CUDA 驱动不修改 buffer 内容。
  return AcquireToken(const_cast<void*>(ptr), size, offset, op);
}

}  // namespace us3_turbo_access::client
