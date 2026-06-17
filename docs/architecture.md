# Us3TurboAccess 项目文档

---

## 1. 综述

Us3TurboAccess 是一个面向 GPU 对象存储的高性能传输 SDK 及其参考网关实现。项目以 NVIDIA GPUDirect Storage（GDS/cuObject）为主数据通路，HTTP/TCP 和 UCX RDMA 为备选通路，提供整对象与分片上传的端到端传输能力。

**核心特性：**

- **多传输通路**：GDS（GPU RDMA）、Native RDMA（UCX RMA WRITE）、HTTP/1.1 三通路可配，运行时按 `DataPath` 枚举路由
- **GPU 直传**：GDS 通路绕过 CPU 内存拷贝，cuObjServer 在网关侧完成 RDMA 操作，GPU buffer 直达后端存储
- **CRC32C 端到端校验**：PUT 时客户端计算校验和、服务端复算验证；GET 时服务端计算、客户端验证
- **分片上传**：SDK 内建 MultipartUpload handle，支持串行 `UploadPart` 和并发 `UploadParts`（固定 worker + fetch-add 流水线模型）
- **会话生命周期管理**：TTL 自动过期 + SessionSweeper 定时清扫，防止会话泄漏
- **线程安全**：Client 对象线程安全；MultipartUpload 单所有者语义

**技术栈：**

- C++20，CMake 3.24+
- brpc（baidu_std + HTTP 协议）
- Protobuf 25.4（静态链接）
- UCX 1.x（RDMA 通路）
- CUDA 12.x/13.x + cuObjServer（GDS 通路）
- OpenSSL / zlib / abseil（brpc 传递依赖）
- spdlog / nlohmann_json

---

## 2. 功能

### 2.1 客户端 SDK（libus3_turbo_access_client）

#### 对象操作

| API | 说明 |
|-----|------|
| `Initialize()` / `Shutdown()` | 初始化/关闭 SDK（幂等） |
| `HeadObject(ObjectId)` | 获取对象元数据（content_length, etag, version） |
| `GetObject(RequestOptions, MutableBufferView)` | 下载对象到指定 buffer |
| `PutObject(RequestOptions, ConstBufferView)` | 上传 buffer 内容为对象 |

#### 异步操作

| API | 说明 |
|-----|------|
| `HeadObjectAsync(...)` | 异步 Head，返回 `std::future` |
| `GetObjectAsync(...)` | 异步 GET |
| `PutObjectAsync(...)` | 异步 PUT |

异步操作通过 `ClientExecutor`（基于 brpc bthread）调度，与 brpc 网络栈共享调度器，避免独立线程池的 mutex 开销。

#### 分片上传

| API | 说明 |
|-----|------|
| `StartUpload(ObjectId, MultipartUpload*, ...)` | 发起分片上传会话 |
| `MultipartUpload::UploadPart(part_number, offset, buffer)` | 上传单个 part |
| `MultipartUpload::UploadParts(parts, concurrency)` | 并发上传多个 part |
| `MultipartUpload::Complete()` | 合并所有 part 为最终对象 |
| `MultipartUpload::Abort()` | 放弃上传，清理资源 |

`UploadParts` 采用固定 worker 数 + `next_index.fetch_add()` 流水线模型，非 batch 模式，worker 完成一个 part 立即抢下一个，无批次边界等待。

#### GPU Buffer 管理

| API | 说明 |
|-----|------|
| `RegisterDeviceBuffer(ptr, size)` | 预注册 GPU buffer 到 cuObj descriptor 表 |
| `UnregisterDeviceBuffer(ptr)` | 释放 descriptor（必须在 `cudaFree` 之前调用） |

#### 并发 GET

大对象自动分片并发下载，按 `parallel_get_threshold` 和 `parallel_get_chunks` 配置触发。GDS 通路额外对齐 4KB GPU page 以优化 cuFile DMA 性能。

### 2.2 网关（us3_turbo_access_gateway）

#### 控制面 RPC（baidu_std 协议）

| RPC | 说明 |
|-----|------|
| `OpenSession` | 创建传输会话，返回 session_id + ticket |
| `HeadObject` | 查询对象元数据 |
| `GdsGet` / `GdsPut` | GDS 通路 chunk 级读写 |
| `StartUpload` / `CompleteUpload` / `AbortUpload` | 分片上传生命周期 |
| `AbortSession` | 终止传输会话 |

#### UCX 数据面 RPC

| RPC | 说明 |
|-----|------|
| `DiscoverRdmaEndpoint` | 网关分配 RDMA buffer，返回地址 + rkey |
| `CommitObject` | RDMA 写入完成后落盘 |
| `CommitPart` | 分片 part 落盘 |
| `AbortSession` | 释放 RDMA buffer |

#### HTTP 入口

| 路由 | 方法 | 说明 |
|------|------|------|
| `/v1/objects/{bucket}/{key}` | GET / PUT / HEAD | 整对象操作 |
| `/v1/uploads/{bucket}/{key}` | POST | 发起分片上传 |
| `/v1/uploads/{upload_id}` | PUT / DELETE | 上传 part / 中止上传 |
| `/v1/uploads/{upload_id}/complete` | POST | 完成分片上传 |
| `/healthz` | GET | 健康检查 |
| `/vars` | GET | bvar 指标面板 |

---

## 3. 框架

### 3.1 整体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        Client SDK                               │
│  ┌──────────┐  ┌──────────────┐  ┌──────────────────────────┐  │
│  │  Client   │  │ MultipartUp- │  │    TransferRouter        │  │
│  │  (facade) │  │ load (handle)│  │  ┌─────┐ ┌─────┐ ┌────┐ │  │
│  └────┬─────┘  └──────┬───────┘  │  │ GDS │ │ HTTP│ │RDMA│ │  │
│       │               │           │  │Path │ │Path │ │Path│ │  │
│       │               │           │  └──┬──┘ └──┬──┘ └─┬──┘ │  │
│       └───────────────┼───────────────┼────────┼────────┤    │  │
│                       │               │        │        │    │  │
│              ClientExecutor(bthread)  │        │        │    │  │
└───────────────────────┼───────────────┼────────┼────────┼────┘
                        │               │        │        │
 ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─╫─ ─ ─ ─ ─ ─ ─╫─ ─ ─ ─╫─ ─ ─ ─╫─ ─
           Network       │   baidu_std   │  HTTP  │  UCX   │
 ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─╫─ ─ ─ ─ ─ ─ ─╫─ ─ ─ ─╫─ ─ ─ ─╫─ ─
                        │               │        │        │
┌───────────────────────┼───────────────┼────────┼────────┼────┐
│                  Gateway                               │    │
│  ┌────────────────────┼───────────────┼────────┼───────┤    │
│  │              API Layer              │        │       │    │
│  │  ControlPlaneService  HttpFrontend  │  RdmaDataPlane   │    │
│  └────────┬───────────┬───────────────┼────────┘       │    │
│           │           │               │                 │    │
│  ┌────────┼───────────┼───────────────┼─────────────────┤    │
│  │         Core Layer  │               │                 │    │
│  │  SessionAppService  │               │                 │    │
│  │  MetadataService    │               │                 │    │
│  │  SessionOpener      │               │                 │    │
│  │  MultipartAppService│               │                 │    │
│  └────────┬───────────┼───────────────┼─────────────────┘    │
│           │           │               │                      │
│  ┌────────┼───────────┼───────────────┼─────────────────┐    │
│  │       Data Path Layer               │                 │    │
│  │  GdsExecutor    HttpExecutor    UcxExecutor          │    │
│  │  GdsMultipart   HttpMultipart   UcxMultipart         │    │
│  └────────┬───────────┬───────────────┬─────────────────┘    │
│           │           │               │                      │
│  ┌────────┴───────────┴───────────────┴─────────────────┐    │
│  │                    Backend (IBackend)                  │    │
│  │           Memory / Null / Composite                   │    │
│  └───────────────────────────────────────────────────────┘    │
│                                                               │
│  ┌──────────────────┐  ┌──────────────────┐                   │
│  │  IoWorkerPool    │  │  SessionSweeper  │                   │
│  │  (N workers)     │  │  (periodic)      │                   │
│  └──────────────────┘  └──────────────────┘                   │
└───────────────────────────────────────────────────────────────┘
```

### 3.2 Client SDK 分层

```
client/
├── include/us3_turbo_access/client/   # 公共 API 头文件
│   ├── client.h                        # Client + MultipartUpload
│   ├── options.h                       # ClientOptions 及子配置
│   ├── result.h                        # Result<T> + Error
│   └── types.h                         # ObjectId, RequestOptions, BufferView, ...
├── src/
│   ├── core/                           # 核心逻辑
│   │   ├── client/                     # Client 实现 + MultipartUpload
│   │   ├── async/                      # ClientExecutor (bthread)
│   │   ├── common/                     # 错误、CRC、通道注册
│   │   ├── contracts/                  # 请求构建（MakeSessionHandshake 等）
│   │   ├── routing/                    # TransferRouter + TransferPath 基类
│   │   ├── gds/                        # GdsTransferPath（并行 GET 4KB 对齐）
│   │   ├── http/                       # HttpTransferPath + when_all
│   │   ├── rdma/                       # RdmaTransferPath（RAII WritePrepared）
│   │   ├── upload/                     # UploadCoordinator（按 DataPath 选 Flow）
│   │   └── metrics/                    # bvar 指标
│   ├── data/                           # 数据面客户端
│   │   ├── gds_data_client.h           # GdsChunk RPC 桩
│   │   ├── http_data_client.h          # HTTP PUT/GET/UploadPart
│   │   └── rdma_data_plane_client.h    # UCX Discover/Commit RPC 桩
│   ├── transports/                     # 传输层
│   │   ├── gds/                        # cuObjClient + GdsMemoryRegistry
│   │   ├── ucx/                        # UcxContext/Worker/Endpoint/EndpointPool
│   │   └── http/                       # brpc Channel 封装
│   └── control/                        # 控制面客户端
│       └── metadata_client.h           # OpenSession/HeadObject/Abort RPC 桩
```

### 3.3 Gateway 分层

```
gateway/
├── include/us3_turbo_access/gateway/  # 公共头文件
│   ├── options.h                       # GatewayOptions
│   ├── result.h                        # Result<T> + MakeError
│   └── types.h                         # 枚举 + 内部类型
├── src/
│   ├── api/                            # RPC/HTTP 协议适配层
│   │   ├── control_plane_service.h/cpp # baidu_std RPC handler
│   │   ├── http_frontend.h/cpp         # HTTP 路由 + handler
│   │   ├── rdma_data_plane_service.h/cpp # UCX 数据面 RPC
│   │   └── conversions.h/cpp           # proto/JSON → 领域对象转换
│   ├── core/                           # 核心业务逻辑
│   │   ├── session/                    # Session 生命周期
│   │   │   ├── session.h               # Session 结构体
│   │   │   ├── session_store.h/cpp     # 分片存储 + TTL
│   │   │   ├── session_app_service.h   # 会话操作门面
│   │   │   ├── session_opener.h/cpp    # OpenSession 流程
│   │   │   └── session_sweeper.h/cpp   # 定时清理
│   │   ├── multipart/                  # 分片上传管理
│   │   │   ├── multipart.h             # MultipartUpload 结构体
│   │   │   ├── multipart_store.h/cpp   # 分片存储 + TTL + MD5 累积
│   │   │   ├── multipart_coordinator.h  # 上传编排
│   │   │   └── multipart_app_service.h # RPC 适配
│   │   └── metadata/                  # 对象元数据
│   ├── data_path/                      # 数据通路执行器
│   │   ├── data_path_executor.h        # IDataPathExecutor 接口
│   │   ├── gds/                        # GDS 通路
│   │   │   ├── gds_executor.h/cpp      # cuObjServer + RDMA + CRC
│   │   │   ├── gds_multipart_path_handler.h/cpp # part 编排
│   │   │   └── buffer_pool.h/cpp       # PinnedBufferPool
│   │   ├── http/                       # HTTP 通路
│   │   │   ├── http_executor.h/cpp      # HTTP PUT/GET 流式
│   │   │   └── http_multipart_path_handler.h/cpp
│   │   └── ucx/                        # UCX RDMA 通路
│   │       ├── ucx_executor.h/cpp       # RMA WRITE + AM 通知
│   │       ├── ucx_multipart_path_handler.h/cpp
│   │       ├── ucx_session_registry.h/cpp # 会话状态管理
│   │       └── ucx_worker.h/cpp         # ProgressLoop 线程
│   ├── backend/                        # 后端存储抽象
│   │   ├── backend.h                   # IBackend 接口
│   │   ├── memory_backend.h/cpp        # 内存实现
│   │   └── null_backend.h/cpp          # 空实现
│   ├── runtime/                        # 运行时基础设施
│   │   ├── gateway_runtime.h/cpp       # 组件装配 + 生命周期
│   │   └── io_worker_pool.h/cpp        # 后台线程池
│   └── common/                         # 共享工具
│       ├── metrics.h/cpp               # bvar 指标定义
│       ├── error.h/cpp                 # 错误码 + ToHttpStatus
│       ├── ids.h/cpp                   # ID 生成（thread_local PRNG）
│       └── crc32c.h/cpp                # CRC32C 计算
└── proto/                              # Protobuf 定义
    ├── control_plane.proto             # 控制面服务
    ├── http_gateway.proto              # HTTP 网关
    └── rdma_data_plane.proto           # UCX 数据面
```

### 3.4 关键设计决策

| 决策 | 原因 |
|------|------|
| 控制面与数据面分离 | 控制面走 brpc baidu_std，数据面按通路特性独立（RDMA/HTTP/GDS） |
| IoWorkerPool 而非 bthread 执行 handler | cuObjServer RDMA 和 backend I/O 是阻塞调用，bthread 阻塞会占住 pthread 池 |
| Session ticket 机制 | 客户端在数据面 RPC 中回传 ticket，网关无状态校验 |
| 分片存储 Session/Multipart | 按 session_id/upload_id 哈希分到 32 个 shard，减少锁争用 |
| 线程局部 PRNG | `MakeRandomId` 使用 `thread_local std::mt19937_64`，避免全局锁 |
| UCX AM 通知替代 flush | 同 ep 上 AM 在 PUT 后发出保证按序到达，无需额外 flush |
| GDS 并行 GET 4KB 对齐 | cuFile DMA 对 GPU page 对齐访问性能更优 |

---

## 4. 交互时序

### 4.1 GDS 整对象 GET

```
 Client                                  Gateway
   │                                        │
   │─── HeadObject(bucket, key) ──────────►│  MetadataService::Head
   │◄── content_length, etag, version ─────│
   │                                        │
   │─── OpenSession(op=GET, path=GDS) ────►│  SessionOpener::Open
   │◄── session_id, ticket, expire_at ─────│
   │                                        │
   │─── GdsGet(session_id, ticket,         │
   │          rdma_token, offset, len) ────►│  PrepareGdsChunk()
   │                                        │  ├─ 校验 gds_executor_ 可用
   │                                        │  ├─ ResolveForGdsChunk()
   │                                        │  ├─ 校验 rdma_token 非空
   │                                        │  └─ BumpActive()
   │                                        │  MetadataService::Head
   │                                        │  GdsExecutor::GetChunk()
   │                                        │  ├─ 从 backend 读取数据
   │                                        │  ├─ RDMA WRITE → 客户端 GPU
   │                                        │  └─ 计算 CRC32C
   │◄── transfer_status, rdma_reply,       │
   │    etag, version, crc32c ─────────────│
```

### 4.2 GDS 整对象 PUT

```
 Client                                  Gateway
   │                                        │
   │─── OpenSession(op=PUT, path=GDS,      │
   │          expected_size=N) ────────────►│  SessionOpener::Open
   │◄── session_id, ticket, expire_at ─────│
   │                                        │  GdsExecutor::OnSessionOpened()
   │                                        │  └─ backend.Reserve(size=N)
   │                                        │
   │─── GdsPut(session_id, ticket,         │
   │          rdma_token, offset, len) ────►│  PrepareGdsChunk()
   │                                        │  GdsExecutor::PutChunk()
   │                                        │  ├─ RDMA READ ← 客户端 GPU
   │                                        │  ├─ 计算 CRC32C
   │                                        │  └─ backend.WriteRange()
   │◄── transfer_status, etag, version ────│
```

### 4.3 HTTP 整对象 PUT（带 CRC32C）

```
 Client                                  Gateway
   │                                        │
   │─── HTTP PUT /v1/objects/b/k ─────────►│  HttpFrontend::HandlePut
   │    Body: <bytes>                       │  ├─ 413 上限检查
   │    x-amz-checksum-crc32c: <base64>    │  ├─ HttpExecutor::Put()
   │                                        │  │  ├─ 计算 CRC32C
   │                                        │  │  ├─ 校验 client CRC == server CRC
   │                                        │  │  └─ backend.Write()
   │◄── 200 OK ───────────────────────────│
   │    ETag: <etag>                        │
   │    x-amz-checksum-crc32c: <base64>    │
```

### 4.4 UCX RDMA PUT（RMA WRITE + AM 通知）

```
 Client                                  Gateway
   │                                        │
   │─── OpenSession(op=PUT, path=RDMA) ──►│  SessionOpener::Open
   │◄── session_id, ticket ────────────────│
   │                                        │
   │─── DiscoverRdmaEndpoint ─────────────►│  UcxExecutor::PrepareTransfer
   │◄── host, ucx_port,                    │  ├─ 分配 pinned buffer
   │    gw_raddr, gw_packed_rkey ──────────│  ├─ ucp_mem_map + ucp_rkey_pack
   │                                        │  └─ 注册 UcxSessionEntry
   │                                        │
   │   ucp_ep_create(ucx_port)             │
   │   ucp_ep_rkey_unpack(packed_rkey)     │
   │   ucp_put_nbx(gw_raddr, data)         │  ◄─ RDMA WRITE 到网关 buffer
   │   ucp_am_send(kAmIdWriteDone) ───────►│  ProgressLoop 收到 AM
   │                                        │  └─ OnAmWriteDone → 置 write_done
   │                                        │
   │─── CommitObject(session_id,           │  UcxExecutor::CommitObject
   │          bytes, client_crc) ──────────►│  ├─ 校验 CRC32C
   │◄── etag, version ────────────────────│  ├─ backend.WriteRange()
   │                                        │  └─ 释放 buffer → pool
```

### 4.5 分片上传（GDS 通路）

```
 Client                                  Gateway
   │                                        │
   │─── StartUpload(bucket, key, GDS) ────►│  MultipartAppService::StartUpload
   │◄── upload_id, max_part_size ──────────│  └─ backend.StartMultipart()
   │                                        │
   │  ┌─ for each part ──────────────────┐ │
   │  │                                   │ │
   │  │ OpenSession(op=PUT, GDS,          │ │  SessionOpener::Open
   │  │   is_multipart_part=true) ────────►│  └─ 跳过整对象 Reserve
   │  │◄── session_id, ticket ────────────│ │
   │  │                                   │ │
   │  │ GdsPut(session_id, rdma_token,   │ │  PrepareGdsChunk()
   │  │   upload_id, part_number, ...) ──►│  GdsMultipartPathHandler::UploadPart
   │  │                                   │ │  ├─ GdsExecutor::PutPart()
   │  │                                   │ │  │  ├─ RDMA READ → pinned buffer
   │  │                                   │ │  │  ├─ backend.WritePart()
   │  │                                   │ │  │  └─ MD5 累积（跨 chunk）
   │  │                                   │ │  └─ coordinator.RegisterPart()
   │  │◄── part_etag, rdma_reply ─────────│ │
   │  │                                   │ │
   │  └───────────────────────────────────┘ │
   │                                        │
   │─── CompleteUpload(upload_id, parts[]) ►│  MultipartAppService::CompleteUpload
   │◄── etag, version, content_length ─────│  └─ backend.CompleteMultipart()
   │                                        │
   │   [失败时] AbortUpload ───────────────►│  backend.AbortMultipart()
```

### 4.6 会话生命周期

```
                          Session 状态机

  ┌─────────┐   BumpActive   ┌────────┐
  │ kOpened │───────────────►│ kActive │◄──── BumpActive
  └────┬────┘                └──┬──┬───┘
       │                        │  │
       │  首次 chunk           │  │ 传输完成
       │ (GdsGet/GdsPut/       │  │
       │  PutChunk/GetChunk)   │  │
       └──────────────────────┘  │
                                 ▼
                          ┌──────────┐
                          │kCompleted│
                          └──────────┘

  任何状态 ──AbortSession──► kFailed
  任何状态 ──TTL 过期─────► kExpired  (SessionSweeper)

  关键约束：
  - BumpActive 刷新 last_activity_at（加锁）
  - SessionSweeper 按 last_activity_at + TTL 判断过期
  - AbortSession 对不存在会话视为 no-op
```
