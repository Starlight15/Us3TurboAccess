# Gateway 设计分析

> Gateway 是面向对象存储的高性能接入端，与 client 配套实现 HTTP / RDMA / GDS 三条通路。本文记录当前 (M1) gateway 的整体架构、启动链路、GDS 流程，以及 GDS 分片上传 (multipart) 流程。

## 一、整体架构

Gateway 采用 **brpc 单端口承载 Protobuf 控制面 + HTTP 数据面**，并在控制面之外挂载 RDMA/GDS 数据通道。代码组织遵循 "runtime → core → data_path → backend" 的分层。

```
                  ┌──────────────────────── GatewayServer (app/) ────────────────────────┐
                  │                                                                       │
                  │   GatewayRuntime  ──── 持有所有组件、负责 Initialize/Run/Shutdown    │
                  │                                                                       │
   brpc port      ├──────────────────────────────────────────────────────────────────────┤
   (8080)         │   API 层                                                             │
   ───────────►   │   ├─ ControlPlaneService   (Protobuf, us3_turbo_access.gateway)        │
                  │   │      OpenSession / HeadObject / GdsGet / GdsPut /                │
                  │   │      Start|Complete|AbortUpload                                  │
                  │   └─ HttpFrontend          (http_master_service)                     │
                  │          HEAD/GET/PUT /v1/objects/{bucket}/{key}, /healthz           │
                  ├──────────────────────────────────────────────────────────────────────┤
                  │   Core 层 (业务逻辑、协议无关)                                       │
                  │   ├─ SessionOpener           创建 session + 调对应 executor 的 hook │
                  │   ├─ SessionStore            64 分片，by_id/by_ticket/by_idempotency │
                  │   ├─ SessionSweeper          后台清过期 session / multipart          │
                  │   ├─ MetadataService         Head / Reserve 转发                     │
                  │   ├─ MultipartStore          32 分片，记录 in-flight upload          │
                  │   └─ MultipartCoordinator    桥接 control-plane 与 backend multipart │
                  ├──────────────────────────────────────────────────────────────────────┤
                  │   Data-path 层  (实现 IDataPathExecutor)                             │
                  │   ├─ HttpExecutor       走 backend.Read/Write，沿 brpc 回应          │
                  │   ├─ GdsExecutor        cuObjServer + PinnedBufferPool               │
   gds_rdma_port  │   └─ (rdma_executor 占位，M2)                                       │
   (rdma+1)  ◄───►│                                                                       │
                  ├──────────────────────────────────────────────────────────────────────┤
                  │   Runtime / Backend                                                  │
                  │   ├─ IoWorkerPool        N 线程，承接重 IO，bRPC worker 不被卡       │
                  │   └─ IBackend (Memory|Null)   未来替换为真后端                       │
                  └──────────────────────────────────────────────────────────────────────┘
```

关键设计选择：

- **统一 executor 抽象**：`IDataPathExecutor` 在 `gateway/src/data_path/data_path_executor.h:24` 收口三类数据通路 (`kHttpTcp`、`kGdsCuObject`、`kNativeRdma`) 的生命周期与 `OnSessionOpened` 协商钩子。SessionOpener 只看接口，不关心 RDMA 细节。
- **控制面/数据面解耦**：HTTP 数据面直接由 `HttpFrontend` 调 `HttpExecutor`，不经 control plane；GDS 必须先 `OpenSession` 走控制面拿 ticket，再用 ticket + rdma_token 调 `GdsGet/GdsPut`。
- **brpc worker 不阻塞**：`GdsGet/GdsPut` 在 `gateway/src/api/control_plane_service.cpp:167` 用 `IoWorkerPool::Submit` 把同步 RDMA 调用下沉到后台线程池。
- **资源 RAII**：`gateway/src/data_path/gds/cuobj_resources.h` 给 cuObjServer 的 channel/buffer/host buffer 都套了 guard，避免 RDMA MR 泄漏。
- **Multipart 索引/字节两阶段**：`MultipartCoordinator` 只管 backend 的索引提交（StartMultipart / CompleteMultipart / AbortMultipart），part 的字节由 GdsExecutor.PutPart 负责，再回调 `RegisterPart` 把 etag 记录到 `MultipartUpload`。

---

## 二、启动链路

入口在 `gateway/src/app/main.cpp:42`，全部走 gflags 配置：

```
main()                                              // 解析 flags，构造 GatewayOptions
 └─ GatewayServer::Start()                          // 委托 runtime 进行 Initialize
                                                    // (app/server.cpp:14)
     └─ GatewayRuntime::Initialize()                // 真正的依赖装配
                                                    // (runtime/gateway_runtime.cpp:50)
         1. EnsureLogger                             // spdlog 初始化（共享 logger 句柄）
         2. 拒绝 rdma_enable=true                    // 原生 RDMA 留给 M2，M1 必须关闭
         3. gds_rdma_port = rdma_port + 1（若为 0）  // 自动避开 RDMA 监听端口
         4. MakeBackend(opts)                       // 构造 Memory / Null backend 桩
         5. SessionStore(gateway_id, public_host:rdma_port,
                         default_chunk_size, ttl)   // 64 分片 session 索引 + chunk plan
         6. MetadataService(*backend)               // Head/Reserve 转发到 backend
         7. HttpExecutor(*backend)                  // HTTP 数据面，永远 available
         8. IoWorkerPool(io_worker_threads)         // 后台线程池，承接重 IO 任务
         9. MultipartStore(ttl)                     // multipart 状态登记表
        10. MultipartCoordinator(backend, store,
                                  max_part_size)    // 协调 backend 索引 + part etag
        11. if (gds_enable)
              GdsExecutor(public_host, bind_host,
                          gds_rdma_port, backend,
                          metadata)                 // 创建 GDS executor
              gds_executor_->Start()                 // 拉起 cuObjServer + buffer pool
                                                    // (gds_executor.cpp:76)
                 ├─ 新建 cuObjServer(bind, port,
                 │                    RDMA_DC_V1)    // 绑定 RDMA 监听端口
                 ├─ isConnected 判定               // 失败返回 kRdmaUnavailable
                 └─ 建 PinnedBufferPool             // {1M, 16M, 256M, 1G} × 4 lease
        12. SessionOpener(sessions, http_exec,
                          gds_exec, rdma=nullptr)   // 注入三类 executor
        13. ControlPlaneService(sessions, metadata,
                                opener, gds_exec,
                                multipart, io_pool) // brpc proto 适配器
        14. HttpFrontend(gateway_id, metadata,
                         http_exec)                 // brpc http_master_service
        15. brpc::ServerOptions { idle_timeout, num_threads,
                                  http_master_service = HttpFrontend }
                                                    // 同端口承载 proto + HTTP
        16. server_.AddService(ControlPlaneService) // 注册控制面 service
        17. server_.Start(bind_host:brpc_port)      // 监听端口
        18. http_frontend_.release()                // brpc 接管所有权，避免双重析构
        19. SessionSweeper(sessions, sweep_interval)// 后台过期清理
              .SetMultipart(store, coordinator)     // 顺带清 multipart
              .Start();
        20. started_ = true; 日志 "gateway ready ..."
 └─ GatewayServer::RunUntilAskedToQuit()            // 阻塞主线程
     └─ brpc::Server::RunUntilAskedToQuit()         // 直到 SIGINT/SIGTERM

Shutdown 反向：SessionSweeper.Stop → server.Stop+Join → io_pool.Stop
                → gds_executor.Stop（先 buffer pool 再 cuObjServer）
                → 其余组件 reset
```

要点：

- **同端口承载两种协议**：`http_master_service` 把 HttpFrontend 挂到 brpc 服务里，与 ControlPlane Protobuf 共用 `brpc_port`。
- **失败早停**：`gds_enable=true` 但 cuObjServer init 失败会直接让 `Initialize()` 失败，进程退出。
- **brpc 接管 HttpFrontend**：`server_.Start` 之后必须 `release()`，否则 Shutdown 时会双重释放（`gateway_runtime.cpp:117`）。

---

## 三、GDS 单段流程

GDS 路径相对客户端是 "控制面分两次 RPC + 数据面 RDMA"。

### 3.1 OpenSession（必经的第一步）

```
client ── OpenSession(bucket, key, op=Get|Put,
                       data_path="gds-cuobject",
                       expected_size, ...) ──► ControlPlaneService::OpenSession
   // 入口：control_plane_service.cpp:117
   │
   ├─ ToOpenSessionParams()                       // proto → core::OpenSessionParams
   │
   └─ SessionOpener::Open(req)                    // 创建 session 并触发执行器 hook
       // session_opener.cpp:31
       ├─ SessionStore::Create(req)               // 注册 session 到三张索引表
       │   // session_store.cpp:39
       │   ├─ 若 idempotency_key 命中            // 幂等路径：直接复用既有 session
       │   ├─ 生成 session_id / ticket /
       │   │   async_handle / expire_at          // 由 common::MakeRandomId 产生
       │   ├─ BuildChunkPlan(offset, expected_size,
       │   │                   default_chunk_size) // 切 8MiB 默认 chunk
       │   ├─ 默认 rdma_parameters = host:port    // 取自 gateway_endpoint
       │   └─ 插入 by_id / by_ticket /
       │       by_idempotency 三个 shard          // 各自按 key 哈希到不同 shard
       │
       ├─ SelectExecutor(DataPath::kGdsCuObject)  // 路由到 GdsExecutor
       │
       └─ GdsExecutor::OnSessionOpened(session)   // GDS 路径专属协商
           // gds_executor.cpp:125
           ├─ 检查 cuObjServer.available()        // 否则 kRdmaUnavailable
           ├─ if (op==Put && expected_size>0)
           │     MetadataService::Reserve(...)    // 让 backend 预占空间
           └─ 返回 RdmaParameters{                  // 通告 GDS RDMA 端点
                  host=public_host,
                  port=gds_rdma_port }

       ↑ SessionOpener 用 hook 返回的 RdmaParameters 覆盖默认值，
         并 SessionStore::UpdateRdmaParameters 回写 store。

返回给客户端：session_id、ticket、gateway_endpoint、chunk_plan[]、rdma_parameters{host,port}
```

### 3.2 GdsGet / GdsPut（每个 chunk 一次 RPC，可并发）

客户端拿到 session 后，按 chunk_plan 逐块发 GdsGet/GdsPut。请求里要带 `session_id`（或 `transfer_ticket`）、`chunk_offset`、`chunk_size`、`rdma_token`（hex 形式编码的远端 GPU buffer 地址）。

#### GdsGet（GPU 读，服务端 → 客户端 GPU）

```
ControlPlaneService::GdsGet                        // 入口 RPC
   // control_plane_service.cpp:158
   │
   └─ io_pool_.Submit([...] {                      // 离开 brpc worker，进入 IO 线程池
        brpc::ClosureGuard done_guard(done);       // RPC 结束保证 done 必然 fire
        ├─ 校验 gds_executor_->available()         // 服务可用性
        ├─ ResolveSession(sessions, request)       // 按 session_id 或 ticket 找回 session
        ├─ 校验 rdma_token 非空                    // 没有客户端 GPU 地址就拒绝
        ├─ MetadataService::Head(bucket, key)      // 取 etag/version，回填给客户端
        └─ GdsExecutor::GetChunk(session,
                                  rdma_token,
                                  offset, length)   // 真正搬数据
              // gds_executor.cpp:156
              ├─ length > 1 GiB → kBadRequest      // cuObjServer 单次硬限
              ├─ PinnedBufferPool::Acquire(length) // 拿到 pin 过且注册 MR 的 host buffer
              ├─ backend_.Read(bucket, key,
              │                  offset, staging)  // 对象存储 → host pinned buffer
              ├─ server.allocateChannelId() +
              │   ChannelGuard                      // 每次传输独占 channel，RAII 释放
              ├─ ParseRemoteBufferAddress(token)   // hex 段 → 远端 GPU 地址
              └─ server.handleGetObject(            // cuObjServer 发起 RDMA WRITE
                    object_id, mr, remote_addr,
                    size, rdma_token, channel)     // 把 host buffer 推到客户端 GPU
       })
   FillGdsResponse(gateway_id, "completed",
                    "gds-cuobject-rdma-write",
                    etag, version)                 // 回填 selected_gateway/etag/...
```

数据流向：`backend → host pinned buffer →(RDMA WRITE)→ client GPU buffer`。

#### GdsPut（GPU 写，客户端 GPU → 服务端，未启用 multipart）

```
ControlPlaneService::GdsPut                        // 入口 RPC
   // control_plane_service.cpp:212
   │
   └─ io_pool_.Submit([...] {                      // 同样下沉到 IO 线程池
        ├─ 校验 available / session / rdma_token  // 与 GdsGet 一致
        ├─ if (request->upload_id().empty())       // ⇐ 单段路径分支
        └─ GdsExecutor::PutChunk(session,
                                  rdma_token,
                                  offset, length)   // 单段 PUT
              // gds_executor.cpp:229
              ├─ length > 1 GiB → kBadRequest
              ├─ length == 0   → backend_.WriteRange(0, {}, total_size)
              │                                    // 空 chunk 仅落索引
              ├─ PinnedBufferPool::Acquire(length) // 拿 pin 过的 host buffer
              ├─ allocateChannelId() + ChannelGuard
              └─ server.handlePutObject(            // cuObjServer 发起 RDMA READ
                    object_id, mr, remote_addr,
                    length, rdma_token, channel)   // 客户端 GPU → host pinned buffer
              └─ backend_.WriteRange(bucket, key,
                                      offset, view,
                                      total_size)   // host buffer 落 backend
       })
   FillGdsResponse(gateway_id, "completed",
                    "gds-cuobject-rdma-read",
                    etag, version)
```

数据流向：`client GPU buffer →(RDMA READ)→ host pinned buffer → backend`。

### 3.3 关键不变量与设计点

- **rdma_token**：客户端把远端 GPU buffer 地址用 hex 编码塞在 token 里，服务端 `ParseRemoteBufferAddress` 取冒号前的 hex 段（`gds_executor.cpp:39`）解出地址，剩余部分（`rkey` 等）原样下发给 cuObjServer。
- **PinnedBufferPool**：4 个 size class（1M/16M/256M/1G），每个 class 最多缓存 4 条 lease；超过最大 class 直接 alloc 临时 buffer，归还时销毁。命中/缺失/oversize 都有 bvar 计数。
- **1 GiB 硬限**：cuObjServer 单次传输不超过 1 GiB；超过让客户端在 chunk_plan 阶段就切好，或走 multipart 把每个 part 分摊在多次 RPC 里。
- **Session 状态机对 GDS 半失效**：`SessionState::kClaimed/kActive` 留给 M2 的原生 RDMA；GDS 路径只用 `kOpened → kCompleted/kFailed/kExpired`，sweep 由 SessionSweeper 周期触发 `SessionStore::SweepExpired`。
- **brpc 不参与重活**：所有 GDS RPC 在 io_pool 里跑，避免 cuObjServer 同步调用把 brpc worker 全部占住。

---

## 四、GDS 分片上传 (Multipart) 流程

当客户端上传一个超大对象（> 1 GiB 或希望并发多 part）时，走 multipart 通路：先一次性 `StartUpload` 拿 `upload_id` 与 `max_part_size`，然后客户端按 part 切分，每个 part 仍走 `OpenSession + GdsPut`，但 `GdsPut` 请求里多带 `upload_id` + `part_number`。最后用 `CompleteUpload(parts[])` 让 backend 拼出完整对象索引；中途出错走 `AbortUpload`。

涉及到的核心组件：

- `core::multipart::MultipartStore`：分片登记表（32 shard，by_id 索引），保存 `MultipartUpload`（含 `upload_id`、`backend_upload_id`、part 进度 map、`State` 与 `last_activity_at`）。
- `core::multipart::MultipartCoordinator`：协调控制面 RPC 与 backend 索引层（StartMultipart / WritePart / CompleteMultipart / AbortMultipart），并提供 `RegisterPart` 让数据面回填 etag。
- `data_path::gds::GdsExecutor::PutPart`：part 字节通道，与 `PutChunk` 几乎对偶，但落地时调 `backend_.WritePart(upload_id, part_number, ...)`。

### 4.1 StartUpload（控制面，索引层创建）

```
client ── StartUpload(bucket, key,
                       expected_total_size,
                       data_path,
                       idempotency_key) ──► ControlPlaneService::StartUpload
   // control_plane_service.cpp:280
   │
   ├─ 把 proto 字段拷成 StartUploadParams        // 0 视为 "未知"，转 std::nullopt
   │
   └─ MultipartCoordinator::StartUpload(params)   // 真正的索引创建
       // multipart_coordinator.cpp:19
       ├─ backend_.StartMultipart(bucket, key,
       │                          total_size_hint) // backend 层创建索引，返回 backend_upload_id
       ├─ MultipartStore::Create(cp)              // 在 store 里登记一条 MultipartUpload
       │     // 生成 gateway 侧 upload_id；记录 backend_upload_id
       │     // state = kActive，last_activity_at = now
       └─ 返回 { upload_id, max_part_size }       // max_part_size 来自 coordinator 配置
返回给客户端：upload_id（gateway 内 ID），max_part_size（每个 part 字节上限）
```

**注意**：客户端拿到的 `upload_id` 是 gateway 自己生成的，不暴露 backend 的 `backend_upload_id`；后续 RPC 传 gateway upload_id，gateway 在 `Lookup` 后内部转换成 backend_upload_id 调 backend。

### 4.2 每个 part：OpenSession + GdsPut(with upload_id, part_number)

每个 part 都需要单独跑一次 `OpenSession` 拿 ticket（或复用同一个 session，由客户端策略决定），但 `GdsPut` 请求里要把 `upload_id` 和 `part_number` 一起带上。控制面据此分流到 `PutPart` 路径：

```
ControlPlaneService::GdsPut                        // 同入口
   // control_plane_service.cpp:212
   │
   └─ io_pool_.Submit([...] {
        ├─ 校验 available / session / rdma_token
        ├─ if (request->upload_id().empty())       // ⇐ 走 3.2 单段路径
        └─ else (multipart 分支)
            ├─ MultipartCoordinator::Lookup(
            │       upload_id)                      // 查 MultipartStore，拿 shared_ptr
            │   // multipart_coordinator.cpp:113
            │   // 取出 backend_upload_id 备用
            │
            ├─ GdsExecutor::PutPart(session,
            │                       rdma_token,
            │                       backend_upload_id,
            │                       part_number,
            │                       chunk_offset,
            │                       chunk_size)     // RDMA + backend.WritePart
            │   // gds_executor.cpp:302
            │   ├─ length > 1 GiB → kBadRequest
            │   ├─ length == 0   → backend_.WritePart(...)  // 空 part 仅占编号
            │   ├─ PinnedBufferPool::Acquire(length)
            │   ├─ allocateChannelId() + ChannelGuard
            │   ├─ server.handlePutObject(...)              // 客户端 GPU → host buffer
            │   └─ backend_.WritePart(upload_id,            // 注意这里是 backend_upload_id
            │                          part_number,
            │                          object_offset,
            │                          view)                // 返回 part_etag
            │
            └─ MultipartCoordinator::RegisterPart(
                    *upload, part_number,
                    chunk_offset, chunk_size,
                    part_etag)                      // 回填 PartProgress 到 MultipartUpload
                // multipart_coordinator.cpp:124
                // 持有 upload->mu 锁，刷新 last_activity_at
       })
   FillGdsResponse(gateway_id, "completed",
                    "gds-cuobject-rdma-read",
                    part_etag, /*version=*/"")     // 把 part_etag 回给客户端
```

数据流向（每个 part 一次）：
`client GPU buffer →(RDMA READ)→ host pinned buffer → backend.WritePart`，并把 `(part_number, etag)` 记入 `MultipartUpload.parts`。

### 4.3 CompleteUpload（合并索引）

客户端上完所有 part，把 `(part_number, etag)` 列表回传：

```
client ── CompleteUpload(upload_id,
                          parts=[(part_number, etag), ...])
                        ──► ControlPlaneService::CompleteUpload
   // control_plane_service.cpp:305
   │
   ├─ 把 proto.parts 拷成 vector<backend::PartRecord>
   │
   └─ MultipartCoordinator::CompleteUpload(upload_id, parts)
       // multipart_coordinator.cpp:46
       ├─ store_.Find(upload_id)                  // 找到 MultipartUpload
       │     // 不存在 → kNotFound
       ├─ scoped_lock(upload->mu)                 // 校验状态
       │     // state != kActive → kBadRequest
       │     // 取出 backend_upload_id
       ├─ backend_.CompleteMultipart(
       │       backend_upload_id, parts)          // backend 拼对象索引，返回 ObjectMetadata
       ├─ upload->state = kCompleted              // 标记完成
       ├─ store_.Erase(upload_id)                 // 从 store 移除
       └─ 返回 ObjectMetadata { etag,             // 客户端拿到最终对象的 etag/version/size
                                version,
                                content_length }
```

### 4.4 AbortUpload（放弃）

```
client ── AbortUpload(upload_id) ──► ControlPlaneService::AbortUpload
   // control_plane_service.cpp:330
   │
   └─ MultipartCoordinator::AbortUpload(upload_id)
       // multipart_coordinator.cpp:85
       ├─ store_.Find(upload_id)                  // 不存在直接返回 false（幂等）
       ├─ scoped_lock(upload->mu)
       │     state = kAborted；取 backend_upload_id
       ├─ backend_.AbortMultipart(backend_upload_id) // 让 backend 释放已写部分
       └─ store_.Erase(upload_id)
```

### 4.5 SessionSweeper 兜底清理

`SessionSweeper` 在 sweep 周期里会顺带扫 `MultipartStore`：

```
SessionSweeper::Run()                              // 周期循环
   // session_sweeper.cpp
   ├─ SessionStore::SweepExpired(now)              // 清过期 session
   └─ if (multipart_store_ && multipart_coordinator_)
        ├─ MultipartStore::SweepExpired(now,       // 按 last_activity_at + ttl 过期
        │                                expired)
        └─ for upload in expired:
              upload->state = kAborted
              MultipartCoordinator::AbortBackend(   // best-effort 调 backend.AbortMultipart
                  upload->backend_upload_id)
        common::metrics().multipart_swept_total << removed
```

这保证客户端 crash / 网络断开后，gateway 与 backend 都不会留下游离的 multipart 索引。

### 4.6 关键不变量

- **upload_id 双层**：客户端看到的是 gateway 生成的 `upload_id`，backend 那一层是独立的 `backend_upload_id`；只有 coordinator/data-path 在内部能看到后者。
- **Coordinator 不持有 IO 线程**：所有耗时调用（`backend.WritePart`、`server.handlePutObject`）都跑在调用者线程，控制面 PutPart 已经在 `io_pool` 里，所以 coordinator 直接同步调用即可。
- **Part 顺序无要求，编号要求唯一**：`MultipartUpload.parts` 是 `std::map<part_number, PartProgress>`，CompleteUpload 时由客户端给定的顺序决定最终拼接。
- **CompleteUpload 必须 state==kActive**：避免重复完成或在已 abort 的 upload 上 complete。

---

## 五、几个值得注意的边界

1. `OpenSession` 没有进入 io_pool，是同步路径——因为它本身只 touch 内存 store 与 metadata.Reserve；如果未来 Reserve 变重（真后端的 multipart init）需要再下沉。
2. `gateway_runtime.cpp:88` 把 `rdma_executor` 写死为 `nullptr`，M2 的原生 RDMA executor 会插在这里。
3. `gds_rdma_port` 默认 = `rdma_port + 1`，两个端口在 M1 不会冲突（rdma_enable 一定为 false），但配置时若手动指定要避开 brpc_port。
4. `StartUpload / CompleteUpload / AbortUpload` 全部在 brpc worker 里同步执行，没有进 io_pool；它们对 backend 的调用预期是轻量索引操作，如果未来 backend 把这些做重，需要照搬 `GdsPut` 的下沉模式。
