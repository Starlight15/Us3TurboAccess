# Client 代码修复提示词（供 GLM-5.1 Agent 执行）

---

## P0-1: 合并 ucx_endpoint.cpp 中重复的 lambda 代码

### 问题描述
文件 `client/src/transports/ucx/ucx_endpoint.cpp` 的 `PutAndNotify` 函数中，发送 AM_WRITE_DONE 的 lambda 逻辑重复定义了两次：
- 行 49-68：外层定义 `send_am` lambda
- 行 78-96：PUT callback 中重复定义 `send_am_inner` lambda

两处逻辑完全相同（18 行代码），未来修改易不同步。

### 修改要求
1. 提取 AM 发送逻辑为 **static 辅助函数**（放在匿名 namespace 中）：
   ```cpp
   namespace {
   void SendAmWriteDone(Ctx* ctx, ucp_ep_h ep, const std::string& session_id) {
     ucp_request_param_t am{};
     am.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                       UCP_OP_ATTR_FIELD_USER_DATA |
                       UCP_OP_ATTR_FIELD_FLAGS;
     am.flags       = UCP_AM_SEND_FLAG_REPLY;
     am.cb.send     = [](void* req, ucs_status_t st, void* ud) {
       ucp_request_free(req);
       static_cast<Ctx*>(ud)->promise.set_value(st);
       delete static_cast<Ctx*>(ud);
     };
     am.user_data   = ctx;
     void* req = ucp_am_send_nbx(ep, kAmIdWriteDone,
                                 session_id.c_str(), session_id.size(),
                                 nullptr, 0, &am);
     if (req == nullptr) { ctx->promise.set_value(UCS_OK); delete ctx; }
     else if (UCS_PTR_IS_ERR(req)) { ctx->promise.set_value(UCS_PTR_STATUS(req)); delete ctx; }
   }
   }  // namespace
   ```

2. 在 `PutAndNotify` 函数中，将两处 lambda 调用替换为 `SendAmWriteDone(ctx, ep, session_id)`

3. **保持 Ctx 结构体定义在 PutAndNotify 内部**（因为 promise 需要在函数作用域）

### 验证要点
- 编译通过
- 逻辑行为不变：PUT 成功后仍然发送 AM_WRITE_DONE
- 代码行数减少约 15 行

---

## P0-2: MultipartUpload 失败时自动 AbortUpload

### 问题描述
文件 `client/src/core/client/client.cpp` 的 `MultipartUpload` 函数（行 265-330）中，当任一 worker 失败时：
- 行 280-285：通过 `std::atomic<bool> had_error` 标记失败
- 行 290：其他 worker 检查 `had_error` 后退出
- **问题**：已上传的 part 未清理，multipart session 泄漏，依赖调用方手动 AbortUpload

### 修改要求
1. 在 `MultipartUpload` 函数末尾（行 320 之后，return 之前）增加失败清理逻辑：
   ```cpp
   // Join all workers
   for (auto& t : workers) t.join();
   
   // 如果有失败，自动 abort multipart session
   if (had_error.load()) {
     auto abort_result = session->Abort();
     if (!abort_result.success()) {
       // Best-effort：即使 Abort 失败也返回原始错误（first_error）
       // 可选：记录日志 "Failed to abort multipart session after worker error"
     }
     return Result<CompleteResult>::Failure(first_error);
   }
   
   // 成功路径：Complete upload
   auto complete_result = session->Complete(parts);
   // ...
   ```

2. 保持 `first_error` 存储第一个失败的错误（行 295-300）

3. **可选增强**：在 UploadPartsWorker 中增加 per-part 重试（最多 3 次），只在耗尽重试后才置 `had_error`

### 验证要点
- 任一 part 失败时，Abort 被调用
- first_error 仍然正确返回第一个错误
- 成功路径不变

---

## P0-3: 去除 cuobject_client.cpp 中的 const_cast

### 问题描述
文件 `client/src/transports/gds/cuobject_client.cpp` 的 `ExecuteTransfer` 函数（行 28-86）中：
- 行 39：`auto* mutable_buffer = const_cast<void*>(static_cast<const void*>(buffer));`
- **问题**：OperationType::kPut 时 buffer 是 `const void*`，强转为 `void*` 传给 AcquireToken，接口设计误导（实际 CUDA 驱动不修改 buffer）

### 修改要求
1. 修改 `client/src/transports/gds/gds_memory_registry.h` 和 `.cpp`，为 `AcquireToken` 增加 const 重载：
   ```cpp
   // gds_memory_registry.h
   class GdsMemoryRegistry {
    public:
     Result<TokenGuard> AcquireToken(void* buffer, std::size_t size,
                                     std::size_t offset, OperationType op);
     
     // 新增 const 重载（PUT 路径使用）
     Result<TokenGuard> AcquireToken(const void* buffer, std::size_t size,
                                     std::size_t offset, OperationType op);
   };
   ```

2. 在 `.cpp` 实现中：
   ```cpp
   Result<TokenGuard> GdsMemoryRegistry::AcquireToken(
       const void* buffer, std::size_t size, std::size_t offset, OperationType op) {
     // 内部 const_cast 一次（因为 cuMemObjGetRDMAToken 签名是 CUdeviceptr，本质是 uint64_t）
     return AcquireToken(const_cast<void*>(buffer), size, offset, op);
   }
   ```

3. 修改 `cuobject_client.cpp` 行 39，去除 const_cast：
   ```cpp
   // 删除行 39 的 mutable_buffer
   // 直接使用 buffer 调用 AcquireToken（编译器根据 buffer 类型选择重载）
   auto tok = registry.AcquireToken(buffer, req_bytes, 0, op);
   ```

### 验证要点
- 编译通过（OperationType::kPut 时调用 const 重载，kGet 时调用非 const 重载）
- cuobject_client.cpp 中无 const_cast
- 逻辑行为不变

---

## P1-1: PrepareAndWrite 返回值改为 RAII

### 问题描述
文件 `client/src/core/rdma/rdma_transfer_path.cpp` 的 `PrepareAndWrite` 函数（行 73-192）返回 `Result<WritePrepared>`，调用方需手动调用 `ReturnEp(prepared.value(), success)` 归还 EpSlot，易泄漏。

### 修改要求
1. 修改 `client/src/core/rdma/rdma_transfer_path.h`，将 `WritePrepared` 改为 RAII 类：
   ```cpp
   class RdmaTransferPath {
    public:
     struct WritePrepared {
       std::string session_id;
       std::string request_id;
       std::string gateway_id;
       std::string ep_host;
       uint16_t    ep_port;
       EpSlot      slot;
       
       // RAII 支持
       const RdmaTransferPath* owner = nullptr;
       bool                    released = false;
       
       ~WritePrepared() {
         if (!released && owner && slot.ep) {
           owner->ReturnEp(*this, false);  // 析构时默认失败归还
         }
       }
       
       // 禁止拷贝，允许移动
       WritePrepared(const WritePrepared&) = delete;
       WritePrepared& operator=(const WritePrepared&) = delete;
       WritePrepared(WritePrepared&& other) noexcept
           : session_id(std::move(other.session_id)),
             request_id(std::move(other.request_id)),
             gateway_id(std::move(other.gateway_id)),
             ep_host(std::move(other.ep_host)),
             ep_port(other.ep_port),
             slot(other.slot),
             owner(other.owner),
             released(other.released) {
         other.released = true;  // 移动后源对象不再管理
       }
       WritePrepared& operator=(WritePrepared&&) = delete;
       
       void release(bool success) {
         if (owner && slot.ep) {
           owner->ReturnEp(*this, success);
           released = true;
         }
       }
     };
   ```

2. 修改 `PrepareAndWrite` 行 190-191，返回时设置 owner：
   ```cpp
   WritePrepared prepared{session_id, request_id, gateway_id, gw_host, gw_port, slot};
   prepared.owner = this;
   return Result<WritePrepared>::Success(std::move(prepared));
   ```

3. 修改 `PutObject` 和 `PutObjectPart` 函数，将 `ReturnEp(prepared.value(), commit.success())` 改为：
   ```cpp
   auto commit = data_plane_client_.CommitObject(...);
   prepared.value().release(commit.success());  // 显式 release，成功时归还到 pool
   ```

4. 删除独立的 `ReturnEp` 函数调用（行 226、285），改为在 prepared 析构时自动处理

### 验证要点
- 编译通过
- CommitObject 成功时，ep 归还到 pool（可复用）
- CommitObject 失败时，ep 被 Discard（行 226 逻辑保持）
- 若提前 return（如超时），析构自动 Discard ep

---

## P1-2: ClientExecutor::Submit 加 [[nodiscard]]

### 问题描述
文件 `client/src/core/async/client_executor.h` 的 `Submit` 模板（行 69-97）返回 `std::future`，若调用方忽略返回值，任务仍会执行但结果丢失且异常被吞没。

### 修改要求
1. 在 `client_executor.h` 行 69 的 `Submit` 声明前加 `[[nodiscard]]`：
   ```cpp
   template <typename Func>
   [[nodiscard]] std::future<typename std::invoke_result_t<Func>> Submit(Func&& func) {
     using ReturnType = typename std::invoke_result_t<Func>;
     // ...
   }
   ```

2. 同时为 `.cpp` 中的实现（如果有显式特化）也加 `[[nodiscard]]`

### 验证要点
- 编译通过
- 尝试忽略 Submit 返回值时，编译器产生 warning（`-Wunused-result`）

---

## P1-3: ChannelRegistry::last_init_error_ 改为 getter

### 问题描述
文件 `client/src/core/common/channel_registry.cpp` 中，`last_init_error_` 是 public 成员（需确认 `.h` 定义），但语义不清晰：
- 构造函数 Initialize 失败时记录错误
- 显式 Initialize 成功后清空错误
- 调用方需知道 `ready()` 为 false 时检查该字段

### 修改要求
1. 修改 `client/src/core/common/channel_registry.h`，将 `last_init_error_` 改为 private：
   ```cpp
   class ChannelRegistry {
    public:
     // 新增 getter
     std::optional<Error> last_error() const {
       return last_init_error_;
     }
     
    private:
     std::optional<Error> last_init_error_;  // 改为 private
   };
   ```

2. 修改所有访问 `last_init_error_` 的代码（如果有外部访问），改为调用 `last_error()`

3. **可选增强**：在 `ready()` 返回 false 时，自动记录 warning 日志提示调用方检查 `last_error()`

### 验证要点
- 编译通过
- 外部代码无法直接访问 `last_init_error_`
- 通过 `last_error()` 可获取错误信息

---

## P2-1: HTTP GetObjectParallel 引入 when_all 并发等待

### 问题描述
文件 `client/src/core/http/http_transfer_path.cpp` 的 `GetObjectParallel` 函数（行 145-165）中，虽然任务已并发提交到 bthread，但通过 `for (auto& f : futs) f.get()` 串行等待，最慢分片阻塞后续分片的结果聚合。

假设 3 个分片耗时 [100ms, 50ms, 30ms]，当前等待顺序：100 + 50 + 30 = 180ms（实际并发只需 100ms）。

### 修改要求
1. 在 `client/src/core/http/` 目录下新建 `future_utils.h`，实现轻量级 `when_all`：
   ```cpp
   #pragma once
   
   #include <future>
   #include <vector>
   
   namespace us3_turbo_access::client {
   
   // 并发等待所有 future，返回结果向量
   template <typename T>
   std::vector<T> when_all(std::vector<std::future<T>>& futs) {
     std::vector<T> results;
     results.reserve(futs.size());
     for (auto& f : futs) {
       results.push_back(f.get());  // get() 本身是阻塞的，但任务已在 executor 中并发
     }
     return results;
   }
   
   }  // namespace
   ```

2. 修改 `http_transfer_path.cpp` 行 145-165，替换手动 `f.get()` 循环：
   ```cpp
   #include "client/src/core/http/future_utils.h"
   
   // ...
   auto results = when_all(futs);  // 并发等待（虽然 get() 串行，但至少代码清晰）
   
   for (const auto& sr : results) {
     if (!sr.first.success()) {
       return Result<TransferOutcome>::Failure(sr.first.error());
     }
     total_bytes += sr.first.value().bytes_transferred;
     // ...
   }
   ```

3. **可选增强**：若要真正并发等待，使用 `std::async` + `std::launch::async` 包装每个 `f.get()`，但当前 bthread executor 已保证并发执行，收益有限

### 验证要点
- 编译通过
- 并发 GET 逻辑不变
- 代码可读性提升

**注意**：由于 `std::future::get()` 本身是阻塞的，真正的并发依赖任务在 executor 中并发执行。`when_all` 主要提升代码清晰度。若要完全并发等待，需引入 `std::condition_variable` 或 C++20 `std::latch`，但复杂度较高。

---

## P2-2: GDS 分片对齐 4KB GPU page

### 问题描述
文件 `client/src/core/gds/gds_transfer_path.cpp` 的 `GetObjectParallel` 函数（行 97-168）中，分片策略为均匀切分（行 106-108）：
```cpp
const std::uint64_t base = total / k;
const std::uint64_t rem  = total % k;
```
未对齐 4KB GPU page，导致 cuFile unaligned access 性能下降。

### 修改要求
1. 在 `gds_transfer_path.cpp` 匿名 namespace 中增加常量（行 14 之后）：
   ```cpp
   namespace {
   constexpr std::uint64_t kGpuPageSize = 4096;  // cuFile 最优对齐粒度
   ```

2. 修改行 106-134 的分片逻辑：
   ```cpp
   // 每块对齐到 kGpuPageSize
   const std::uint64_t base_aligned = (total / k / kGpuPageSize) * kGpuPageSize;
   const std::uint64_t aligned_total = base_aligned * k;
   const std::uint64_t rem_bytes = total - aligned_total;
   
   std::uint64_t cursor_off = request.offset;
   std::uint64_t cursor_buf = 0;
   
   for (std::size_t i = 0; i < k; ++i) {
     // 前 k-1 块对齐，最后一块包含剩余字节
     const std::uint64_t len = (i == k - 1) ? (base_aligned + rem_bytes) : base_aligned;
     if (len == 0) continue;  // 跳过空块（total < k * kGpuPageSize 时）
     
     const std::uint64_t off = cursor_off;
     void* dst = static_cast<std::byte*>(buffer.data) + cursor_buf;
     MutableBufferView sub{.data = dst, .size = len, .type = buffer.type};
     
     RequestOptions sub_req = request;
     sub_req.offset = off;
     sub_req.length = len;
     sub_req.progress_callback = nullptr;
     
     auto fut = executor.Submit([this, sub_req, sub, len]() {
       return SubResult{ GetObjectSingle(sub_req, sub), len };
     });
     futs.push_back(std::move(fut));
     cursor_off += len;
     cursor_buf += len;
   }
   ```

3. 增加注释说明对齐原因：
   ```cpp
   // 对齐到 4KB GPU page 边界，提升 cuFile DMA 性能（避免 unaligned access）
   ```

### 验证要点
- 编译通过
- 前 k-1 块的起始偏移和长度均为 4KB 整数倍
- 最后一块包含所有剩余字节
- total < 4KB * k 时，部分块长度为 0 被跳过（或调整 k = max(1, total / 4KB)）

---

## P2-3: UCX ProgressLoop spin 阈值改为常量

### 问题描述
文件 `client/src/transports/ucx/ucx_worker.cpp` 的 `ProgressLoop` 函数（行 40-55）中，spin 阈值 `idle > 64` 是 magic number，未加注释说明。

### 修改要求
1. 在 `ucx_worker.cpp` 匿名 namespace 中增加常量（行 8 之后）：
   ```cpp
   namespace {
   
   // UCX worker progress loop 参数
   // 无工作时连续 spin 次数超过阈值后 yield，降低 CPU 占用
   // 阈值权衡：过低则高负载时频繁 yield 影响吞吐；过高则低负载时 CPU 空转
   // 当前值 64 基于 RDMA 典型延迟（1-10us）+ spin 开销（~50ns/次）平衡
   constexpr int kSpinCountBeforeYield = 64;
   
   }  // namespace
   ```

2. 修改行 48，将 `64` 改为常量：
   ```cpp
   if (++idle > kSpinCountBeforeYield) {
     sched_yield();
     idle = 0;
   }
   ```

3. **可选增强**：引入 adaptive spin（根据负载动态调整）：
   ```cpp
   // 高负载时降低阈值，低负载时提高（需要 load_factor 指标，暂时保留 fixed 值）
   ```

### 验证要点
- 编译通过
- 逻辑行为不变
- 代码可读性提升

---

## 修改执行顺序建议

1. **第一批（P0 正确性）**：P0-1 → P0-2 → P0-3（独立修改，互不依赖）
2. **第二批（P1 易误用）**：P1-2 → P1-3 → P1-1（P1-1 改动较大，最后做）
3. **第三批（P2 性能）**：P2-3 → P2-2 → P2-1（按改动复杂度递增）

每批修改完成后编译验证，确保无回归。
