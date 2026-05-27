#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

struct CuObjApi;

/**
 * @brief 拿到一段 RDMA token 的 RAII 持有者。
 *
 * token 是 cuObj SDK 分配的 C 字符串，进 RPC 发给 gateway 后必须释放。
 * gateway 端 RDMA 完成 + RPC 响应到达 = token 可释放。析构时调
 * cuMemObjPutRDMAToken；移动构造 / 赋值后老的实例 valid()==false。
 */
class GdsRdmaToken {
 public:
  GdsRdmaToken() = default;
  GdsRdmaToken(GdsRdmaToken&& other) noexcept;
  GdsRdmaToken& operator=(GdsRdmaToken&& other) noexcept;
  GdsRdmaToken(const GdsRdmaToken&) = delete;
  GdsRdmaToken& operator=(const GdsRdmaToken&) = delete;
  ~GdsRdmaToken();

  [[nodiscard]] std::string_view str() const noexcept;
  [[nodiscard]] bool             valid() const noexcept { return tok_ != nullptr; }

 private:
  friend class GdsMemoryRegistry;
  GdsRdmaToken(const CuObjApi* api, void* client, char* tok) noexcept
      : api_(api), client_(client), tok_(tok) {}
  void Reset() noexcept;

  const CuObjApi* api_{nullptr};
  void*           client_{nullptr};
  char*           tok_{nullptr};
};

/**
 * @brief GDS GPU buffer 注册器 / RDMA token 颁发器。
 *
 * 设计：每个 Client 各持一份（per-Client façade），但实际状态落在进程级
 * 单例上 —— cuObj SDK 的 descriptor 表本来就是 process-wide，且我们只起
 * 一个 cuObj client 实例（PoC 已验证多线程 GetRDMAToken 安全）。
 *
 * 用户契约：
 *   - 调 RegisterBuffer(ptr, size) 把 device buffer 一次性 pin 入 BAR1；
 *     idempotent；同一 ptr 重复调直接 success。
 *   - 在 cudaFree(ptr) 之前必须调 UnregisterBuffer(ptr)，否则 nvidia-fs
 *     有死锁风险（NVIDIA 文档明确警告）。
 *   - AcquireToken 是 PUT/GET 数据面内部使用的工厂；用户不应直接调。
 *
 * BufferView::Register 是 V1 时代的入参校验残留，仍然保留只用于 type+size
 * sanity check，跟 descriptor 表无关。
 */
class GdsMemoryRegistry {
 public:
  GdsMemoryRegistry()  = default;
  ~GdsMemoryRegistry() = default;

  GdsMemoryRegistry(const GdsMemoryRegistry&)            = delete;
  GdsMemoryRegistry& operator=(const GdsMemoryRegistry&) = delete;

  // V1 残留：仅做 buffer.type==kCudaDevice / non-null / size>0 校验。
  // 没有副作用，不触碰 cuObj SDK。
  [[nodiscard]] Result<struct RegisteredBuffer>
      Register(OperationType operation, MutableBufferView buffer) const;
  [[nodiscard]] Result<struct RegisteredBuffer>
      Register(OperationType operation, ConstBufferView   buffer) const;

  // === Token-direct 主路径 ===

  // 显式注册：把 device buffer 一次性 pin 入 cuObj descriptor 表。idempotent。
  [[nodiscard]] Result<bool> RegisterBuffer(void* ptr, std::size_t size);

  // 显式注销：必须在 cudaFree(ptr) 之前调。
  [[nodiscard]] Result<bool> UnregisterBuffer(void* ptr);

  // 拿一段 (ptr, offset, size, op) 的 RDMA token；token 是 RAII，析构释放。
  // 若 ptr 尚未注册，会内部 lazy register（但建议提前 RegisterBuffer 避免
  // 数据面上 pin 内存的毫秒级 syscall 抖动）。
  [[nodiscard]] Result<GdsRdmaToken>
      AcquireToken(void* ptr, std::size_t size, std::size_t offset,
                    OperationType op);
};

// 老 API 保留：空 struct，调用方仅用 Result 的成功/失败。
struct RegisteredBuffer {};

}  // namespace us3_turbo_access::client
