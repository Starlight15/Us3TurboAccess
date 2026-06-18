# Backend 代码框架规范

## 一、设计原则

### 1.1 核心定位
- **数据面专职**：处理字节流（RDMA-READ），v1 只实现 GDS 单对象 PUT 丢弃
- **无状态设计**：不校验 session/ticket（proxy 已校验），只验证 token 非空和长度合法
- **资源确定性**：启动时初始化 cuObjServer + PinnedBufferPool，失败即退出
- **最小实现**：v1 不接 IDataStore、不写盘，只返回 crc32c 供端到端校验

### 1.2 代码质量标准
- **资源 RAII**：cuObjServer / PinnedBufferPool 生命周期明确，析构顺序严格
- **错误传播清晰**：用 `DiscardOutcome` 结构体统一返回结果，不抛异常
- **复用 gateway 代码**：直接编译 gateway 源文件，不复制粘贴
- **注释说明复用依赖**：明确哪些文件来自 gateway，为何复用

---

## 二、目录结构

```
backend/
├── CMakeLists.txt              # 编译 gateway 源文件（buffer_pool/cuobj_resources/crc32c）
├── skill.md                    # 本文档
└── src/
    ├── main.cpp                # 入口：先启动 cuObjServer+pool，再启动 brpc
    ├── backend_data_plane_service.h
    ├── backend_data_plane_service.cpp  # RPC 服务层（只实现 GdsPut 单对象分支）
    ├── backend_gds_sink.h
    └── backend_gds_sink.cpp    # GDS 数据面：RDMA-READ + 丢弃
```

### 复用的 gateway 代码（不改动，按路径编译）
```
gateway/src/data_path/gds/buffer_pool.cpp
gateway/src/data_path/gds/cuobj_resources.cpp
gateway/src/common/crc32c.cpp
```

**复用原则**：
- ✅ 按**源文件路径**编译进 backend target（见 CMakeLists.txt）
- ✅ 通过 `target_include_directories` 包含 gateway 头文件
- ❌ 不复制代码到 backend（维护两份会分化）

---

## 三、模块职责

### 3.1 `src/main.cpp`
**职责**：启动顺序管理 + 优雅退出

**强制启动顺序**：
```
1. 解析 gflags（bind_host, rdma_port, brpc_port）
2. BackendGdsSink::Start()  ← 先起 cuObjServer+pool
3. if (!sink.available()) { LOG(FATAL) << "GDS sink 初始化失败"; return 1; }
4. brpc::Server::Start()    ← 再起 RPC 服务
5. server.RunUntilAskedToQuit()
6. server.Stop(); sink.Stop();  ← 逆序退出
```

**规范**：
- 启动失败即 `return 1`，不允许带病启动（避免 RPC 请求打到未就绪的 sink）
- 退出顺序：先停 brpc（拒绝新请求），再停 sink（释放 RDMA 资源）

---

### 3.2 `src/backend_gds_sink.{h,cpp}`
**职责**：
- 拥有 `cuObjServer` + `PinnedBufferPool` 的生命周期
- 执行 RDMA-READ（via `cuObjServer::handlePutObject`）
- 计算 crc32c，丢弃字节，返回 `DiscardOutcome`

**关键设计决策（在注释中说明）**：
```cpp
/**
 * @brief 启动顺序：Start() → available() → ReceiveAndDiscard()
 *
 * 资源顺序约束（来自 cuObjServer 实现）：
 *   1. cuObjServer 先于 PinnedBufferPool 创建（server 负责 RDMA 设备初始化）
 *   2. pool->Shutdown() 必须在 server reset 前（deRegisterBuffer 需 server 存活）
 *
 * 线程安全：
 *   Start()/Stop() 不并发调用（main.cpp 串行调用）。
 *   ReceiveAndDiscard() 可并发（server_/pool_ 在 Start() 后不再修改）。
 */
class BackendGdsSink { /*...*/ };
```

**`ReceiveAndDiscard` 规范**：
```cpp
/**
 * @brief RDMA-READ 拉 length 字节后丢弃，返回 crc32c。
 *
 * 前置条件：available() == true。
 * length==0：视为成功空传输（bytes_transferred=0, crc32c=0）。
 * length > 1GiB：直接返回失败（cuObjServer 单次 RDMA 上限）。
 *
 * @return DiscardOutcome，ok==false 时 error 含 ibv_wc_status 描述。
 */
DiscardOutcome ReceiveAndDiscard(const std::string& object_id,
                                 const std::string& rdma_token,
                                 uint64_t length);
```

---

### 3.3 `src/backend_data_plane_service.{h,cpp}`
**职责**：实现 `DataPlaneService` RPC 接口（薄适配层）

**v1 只实现 `GdsPut`**，其余方法返回 `UNIMPLEMENTED`：
```cpp
/**
 * @brief DataPlaneService RPC 适配层。
 *
 * 职责：参数校验 → 调 sink_.ReceiveAndDiscard() → 返回 etag。
 * 不做：session 校验（proxy 已校验），写盘（v1 丢弃）。
 *
 * 依赖：BackendGdsSink（注入引用，不拥有所有权）。
 */
class BackendDataPlaneService : public DataPlaneService { /*...*/ };
```

**`GdsPut` 实现模式**：
```cpp
void BackendDataPlaneService::GdsPut(
    RpcController* cntl_base,
    const GdsChunkRequest* request,
    GdsChunkResponse* response,
    Closure* done) {
  brpc::ClosureGuard done_guard(done);

  // 1. 参数校验
  if (request->rdma_token().empty()) {
    cntl->SetFailed(EINVAL, "missing rdma_token");
    return;
  }
  if (!request->upload_id().empty()) {
    // GDS 单对象 PUT 不应有 upload_id（分段上传走不同接口）
    cntl->SetFailed(EINVAL, "unexpected upload_id in single PUT");
    return;
  }

  // 2. 调数据面
  auto outcome = sink_.ReceiveAndDiscard(
      BuildObjectId(request->bucket(), request->object_key()),
      request->rdma_token(),
      request->content_length());

  // 3. 返回结果
  if (!outcome.ok) {
    cntl->SetFailed(EIO, outcome.error);
    return;
  }
  response->set_etag(BuildEtag(outcome.crc32c));
  response->set_bytes_received(outcome.bytes_transferred);
}
```

**规范**：
- `BuildObjectId` / `BuildEtag` 提取为文件级静态函数（不暴露到头文件）
- 不在此层启动 bthread（ReceiveAndDiscard 本身是同步调用，直接执行即可）

---

## 四、编写规范

### 4.1 资源管理
**RAII 原则**：
```cpp
// ✅ 用 shared_ptr 管理 cuObjServer / pool
std::shared_ptr<cuObjServer> server_;
std::shared_ptr<PinnedBufferPool> pool_;

// Stop() 中先释放 pool，再释放 server
void Stop() {
  if (pool_) { pool_->Shutdown(); pool_.reset(); }
  if (server_) { server_.reset(); }  // Shutdown 内置在析构
}
```

**禁止**：
- ❌ 裸指针管理 cuObjServer（析构顺序难以保证）
- ❌ 在 RPC handler 中修改 `server_` / `pool_`（只读访问）

### 4.2 错误处理
**统一用 `DiscardOutcome`，不抛异常**：
```cpp
// ✅ 结构体返回，调用方显式检查
DiscardOutcome outcome = sink_.ReceiveAndDiscard(...);
if (!outcome.ok) { cntl->SetFailed(EIO, outcome.error); return; }

// ❌ 不用异常
try { sink_.ReceiveAndDiscard(...); } catch (...) { ... }
```

**错误信息规范**：
- RDMA 失败：包含 `ibv_wc_status` 数值和描述（调试必需）
- 参数校验失败：说明哪个字段，期望值是什么
- 格式：`"rdma read failed: status=IBV_WC_REM_ACCESS_ERR(13)"`

### 4.3 注释规范

**类级注释**（必须）：
- 说明：职责、启动/停止顺序约束、线程安全性
- 说明哪些成员是"启动后恒定不变"的（可无锁并发访问）

**函数注释**（关键函数）：
- 前置条件（如 `available() == true`）
- 返回值语义（`ok=false` 时哪些字段有意义）
- 资源顺序约束（如析构顺序）

**复用 gateway 代码时注释**：
```cpp
// 复用 gateway PinnedBufferPool：管理 RDMA 注册的 pinned 内存，
// deRegisterBuffer 需要 cuObjServer 存活，故 pool_ 先于 server_ 释放。
#include "data_path/gds/buffer_pool.h"
```

### 4.4 不冗余
- `BuildObjectId` / `BuildEtag` 只在 `backend_data_plane_service.cpp` 中定义（文件级 static），不提升到头文件
- v1 不预设多 sink 抽象（只有 GDS 一种，等 HTTP/UCX 需求明确再抽象）
- 不重复 gateway 的常量定义，直接 include gateway 头文件使用

### 4.5 依赖管理
**依赖方向**：
```
main.cpp
  ↓
backend_data_plane_service.cpp
  ↓
backend_gds_sink.cpp
  ↓
gateway: buffer_pool / cuobj_resources / crc32c（复用，只读）
```

**禁止**：
- ❌ `backend_gds_sink.cpp` 依赖 brpc（纯 GDS/RDMA 层，不耦合 RPC 框架）
- ❌ 循环依赖

---

## 五、实施步骤

### P0（已完成，维护规范）
1. ✅ `backend_gds_sink.h/cpp`：ReceiveAndDiscard + crc32c
2. ✅ `backend_data_plane_service.h/cpp`：GdsPut 实现
3. ✅ `main.cpp`：正确启动顺序

### 后续扩展（需求明确后）
- **写盘**：在 `ReceiveAndDiscard` 后接 IDataStore 接口，不改 RPC 层
- **HTTP sink**：新增 `backend_http_sink.h/cpp`，`BackendDataPlaneService` 加 `HttpPut` 方法
- **UCX sink**：同上

---

## 六、检查清单

提交代码前，确认：
- [ ] `Start()` 失败时 `main.cpp` 立即退出（不带病启动）
- [ ] `Stop()` 顺序：pool 先于 server 释放
- [ ] `ReceiveAndDiscard` 的 `DiscardOutcome` 包含 ibv_wc_status 数值
- [ ] `GdsPut` 校验了 `upload_id` 为空（单对象 PUT 约束）
- [ ] 类级注释说明了线程安全性和资源顺序约束
- [ ] 没有复制 gateway 代码（只编译复用）
