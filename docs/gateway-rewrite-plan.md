# Gateway 重写计划

> 此文档是**重写计划**,不是终态设计文档。目标是让评审能拍板 "可以开始动手"。
> 落地后会按里程碑产出代码,设计细节随代码演化。

| 字段 | 值 |
|---|---|
| 文档状态 | 待你确认 |
| 适用范围 | `Us3TurboAccess/gateway/` 全量新写 |
| 风格对齐 | `Us3TurboAccess/client/` 已有的命名/分层/Result 模型 |
| RPC 栈 | brpc(与 client 一致,不引 gRPC)|

---

## 1. 目标 / 非目标

### 1.1 目标

- 第一版只做**基础功能 + 基础性能**,接口与目录稳定到能扩;
- 与 `Us3TurboAccess/client` 协议、命名空间、Result 模型、第三方库一致;
- 后端是内存桩(`MemoryBackend`,可选 `NullBackend`),不持久化;
- 三条数据通路(HTTP / native-rdma / gds-cuobject)路径骨架先全部就位,gds 的真实 RDMA 实现可放 M2;
- 单进程可起、可停、有日志、有 brpc `/vars` 自带指标、单测 + 一个 e2e。

### 1.2 非目标(明确不做,留扩展点)

- 不做对象持久化、副本、版本(后端职责);
- 不做跨节点 HA / 多实例同步;
- 不做多租户公平、限流、QoS;
- 不做 thread-per-core、share-nothing、Mailbox 跨核;
- 不做 DCT / ODP / nvidia-peermem / cuObjServer 替换(GDS 暂复用现有 `cuObjServer` 路径,与旧实现保持兼容);
- 不引 OpenTelemetry / Prometheus(brpc bvar 内建够用)。

---

## 2. 与 client 的一致约定(锁定)

| 维度 | 选择 | 理由 |
|---|---|---|
| Namespace | `us3_turbo_access::gateway` | 与 `us3_turbo_access::client` 对称 |
| C++ 标准 | C++20 | 与 `Us3TurboAccess/CMakeLists.txt` 一致 |
| Result 模型 | `Result<T> = { success, value, error }` + `Error{code, message, retryable, ...}` | 与 client `result.h` 一致(可适配 server 端字段)|
| 错误码 | `ErrorCode` 枚举,沿用 client 已有值 + 服务端增量 | 跨进程语义对齐 |
| Logger | `spdlog::logger` 注入,经 `us3_turbo_access_spdlog` interface | 与 client 一致 |
| 配置 | gflags(brpc 已经依赖)+ JSON(`nlohmann_json`)文件可选 | 不引 TOML,减少三方 |
| RPC | brpc(`us3_turbo_access_brpc`)| 与 client 一致 |
| JSON | `nlohmann_json` | client 已用 |
| Proto | 复用 `FusionAccess/gateway/proto/control_plane.proto`(已在顶层 CMake 中生成)| 直接 link `us3_turbo_access_control_plane_proto` |

---

## 3. 顶层架构

```
                 Client (us3_turbo_access::client)
                            │
              ┌─────────────┼─────────────┐
              ▼             ▼             ▼
       HTTP (brpc       brpc protobuf   RDMA (verbs / rdmacm)
       http_master)     ControlPlane     直连
              │             │             │
              └─────────────┼─────────────┘
                            ▼
                   ┌────────────────┐
                   │  GatewayCore   │  ← 装配/lifecycle
                   └────────┬───────┘
                            │
       ┌────────┬───────────┼───────────┬──────────┐
       ▼        ▼           ▼           ▼          ▼
  SessionStore  TransferEngine  RdmaEngine  Backend  Infra
                                                     (config/log/errors/buffer)
```

**4 个业务模块 + 1 个基础设施**,与 client 镜像对称:

| client | gateway 对应 |
|---|---|
| `ClientCore`(运行时装配) | `GatewayCore` |
| `ObjectApi` / `TransferApi`(public API)| `ObjectService` / `TransferService`(brpc service)+ `HttpFrontend`(brpc http_master)|
| `MetadataClient` / `gds_data_client` / `TransferRouter` | `SessionStore` / `TransferEngine` |
| `transports/gds` / `transports/rdma` | `RdmaEngine`(原生 verbs)+ 兼容 `cuObjServer` |
| 无对应 | `IBackend` + `MemoryBackend` / `NullBackend` |

---

## 4. 目录结构(锁定)

```
Us3TurboAccess/gateway/
├── CMakeLists.txt
├── include/us3_turbo_access/gateway/
│   ├── server.h          ── 顶层壳 GatewayServer
│   ├── options.h         ── GatewayOptions
│   ├── result.h          ── 复用 client/result.h 风格(同语义,独立 namespace)
│   └── types.h
├── src/
│   ├── core/
│   │   ├── server/
│   │   │   ├── gateway_core.{h,cpp}   ── 装配 + Initialize/Shutdown
│   │   │   └── server.cpp             ── GatewayServer 入口实现
│   │   ├── session/
│   │   │   ├── session.{h,cpp}        ── Session 结构体
│   │   │   └── session_store.{h,cpp}  ── 唯一存放点
│   │   ├── transfer/
│   │   │   ├── transfer_engine.{h,cpp}── 路径分发 + 流式编排
│   │   │   ├── http_flow.{h,cpp}      ── HTTP GET/PUT 流
│   │   │   └── rdma_flow.{h,cpp}      ── RDMA GET/PUT 流
│   │   └── common/
│   │       ├── ids.{h,cpp}            ── UUID / ticket 工具
│   │       └── range.{h,cpp}          ── HTTP Range 解析
│   ├── api/                            ── brpc service 接入点(无业务逻辑)
│   │   ├── control_plane_service.{h,cpp}
│   │   └── http_frontend.{h,cpp}      ── brpc http_master_service
│   ├── transports/
│   │   ├── rdma/
│   │   │   ├── rdma_engine.{h,cpp}    ── 单 PD/CQ,每会话 1 QP
│   │   │   └── rdma_listener.{h,cpp}  ── rdma_cm 事件 + CQ poll
│   │   └── gds/
│   │       └── cuobj_session.{h,cpp}  ── 兼容现有 cuObjServer(M2 启用)
│   ├── backend/
│   │   ├── backend.h                  ── IBackend
│   │   ├── memory_backend.{h,cpp}
│   │   └── null_backend.{h,cpp}
│   └── infra/
│       ├── config.{h,cpp}             ── gflags 包一层
│       ├── log.h                      ── spdlog 转发
│       ├── errors.{h,cpp}             ── ErrorCode 枚举与映射
│       ├── buffer.{h,cpp}             ── BufLease + 简单 freelist
│       └── mr_registry.{h,cpp}        ── 启动期注册池(M2 启用)
└── tests/
    ├── unit/
    │   ├── session_store_test.cpp
    │   ├── memory_backend_test.cpp
    │   ├── range_test.cpp
    │   └── http_flow_test.cpp
    └── e2e/
        └── http_round_trip_test.cpp
```

约 30 个文件,M1 完成预计 4k–6k 行 C++,M3 完成 7k–10k 行。

---

## 5. 三方库引入(只引必须的)

复用 `Us3TurboAccess/CMakeLists.txt` 已声明的:

| 库 | 来源 | 用途 |
|---|---|---|
| brpc + protobuf + abseil + leveldb + snappy + gflags + openssl + zlib | `us3_turbo_access_brpc` interface(已配) | RPC、HTTP server、bvar、HTTP client |
| spdlog | `us3_turbo_access_spdlog` interface | 日志 |
| nlohmann_json | `find_package` | JSON 解析与生成 |
| libibverbs / librdmacm | `find_library`(顶层已找)| 原生 RDMA |
| cuobjserver / cudart / cuobjclient 头 / cufile 头 | 顶层已找 | GDS 兼容(M2 启用)|

**不新增任何三方库**。具体不引入的:
- gRPC、OpenTelemetry、prometheus-cpp、folly、toml++、CLI11、simdjson、xxHash、mimalloc、UCX、Boost.Beast、Asio standalone — 全部 v2 真有需要再加。

CMake 一改即可:在顶层增加 `add_subdirectory(gateway)`,gateway 自己的 `CMakeLists.txt` 复用 `us3_turbo_access_brpc` / `us3_turbo_access_spdlog` 与已生成的 proto target,无需重复定义。

---

## 6. 关键 public 头(契约骨架)

只列签名,供评审定型;不展开实现。

### 6.1 `include/us3_turbo_access/gateway/options.h`

```cpp
namespace us3_turbo_access::gateway {

struct GatewayOptions {
  // 网络
  std::string bind_host{"0.0.0.0"};
  std::string public_host{"127.0.0.1"};
  int         brpc_port{8080};         // brpc Server 一个端口承载 HTTP + protobuf
  int         idle_timeout_sec{-1};
  int         num_threads{4};

  // RDMA
  bool        rdma_enable{true};
  std::string rdma_device;             // 空 = 自动选 mlx5_0
  int         rdma_port{18515};
  int         gds_rdma_port{0};        // 0 = rdma_port + 1
  std::size_t default_chunk_size{8U * 1024U * 1024U};

  // 后端
  std::string backend{"memory"};       // "memory" | "null"
  std::size_t backend_capacity{4ULL * 1024ULL * 1024ULL * 1024ULL};

  // 标识
  std::string gateway_id{"us3-turbo-access-gateway"};

  // 日志
  std::shared_ptr<spdlog::logger> logger;
};

}  // namespace us3_turbo_access::gateway
```

### 6.2 `include/us3_turbo_access/gateway/server.h`

```cpp
namespace us3_turbo_access::gateway {

class GatewayCore;

class GatewayServer {
 public:
  explicit GatewayServer(GatewayOptions options);
  ~GatewayServer();

  GatewayServer(const GatewayServer&) = delete;
  GatewayServer& operator=(const GatewayServer&) = delete;

  [[nodiscard]] Result<bool> Start();
  void Shutdown();
  void RunUntilAskedToQuit();           // 包 brpc::Server::RunUntilAskedToQuit

 private:
  std::unique_ptr<GatewayCore> core_;
};

}  // namespace us3_turbo_access::gateway
```

### 6.3 `include/us3_turbo_access/gateway/result.h`

复用 client/result.h 结构(`Result<T>` + `Error{code,message,retryable,request_id,failed_path}`),namespace 改为 gateway。**`ErrorCode` 与 client 共享语义**(同名同值),通过头文件复用或独立声明同值,均可。倾向独立声明 + 文档约束同步。

### 6.4 `core/server/gateway_core.h`(私有)

```cpp
class GatewayCore {
 public:
  explicit GatewayCore(GatewayOptions opts);
  ~GatewayCore();
  Result<bool> Initialize();    // 依赖装配 + 启动 brpc::Server 与 RDMA
  void         Shutdown();
  void         Run();

  // 私有 getter,供 service 层使用
  IBackend&        backend()        { return *backend_; }
  SessionStore&    session_store()  { return *session_store_; }
  TransferEngine&  transfer_engine(){ return *transfer_engine_; }
  RdmaEngine*      rdma_engine()    { return rdma_engine_.get(); }
  const GatewayOptions& options() const { return options_; }

 private:
  GatewayOptions                  options_;
  std::unique_ptr<IBackend>       backend_;
  std::unique_ptr<SessionStore>   session_store_;
  std::unique_ptr<RdmaEngine>     rdma_engine_;          // 可空(降级)
  std::unique_ptr<TransferEngine> transfer_engine_;
  std::unique_ptr<ControlPlaneServiceImpl> control_service_;
  std::unique_ptr<HttpFrontendImpl>        http_frontend_;
  brpc::Server                    server_;
};
```

### 6.5 `core/session/session_store.h`(私有)

```cpp
class SessionStore {
 public:
  explicit SessionStore(std::string gateway_id, std::shared_ptr<spdlog::logger> log);
  Result<std::shared_ptr<Session>> Create(const OpenSessionParams& req);
  Result<std::shared_ptr<Session>> Find(std::string_view session_id) const;
  Result<std::shared_ptr<Session>> Claim(std::string_view ticket);
  Result<bool>                      Commit(std::string_view session_id, const ObjectMeta&);
  Result<bool>                      Cancel(std::string_view session_id);
 private:
  std::shared_ptr<spdlog::logger> logger_;
  std::string                      gateway_id_;
  mutable std::mutex               mu_;
  std::unordered_map<std::string, std::shared_ptr<Session>> by_id_;
  std::unordered_map<std::string, std::string>              by_ticket_;
};
```

### 6.6 `backend/backend.h`(私有)

```cpp
struct ObjectMeta {
  std::size_t content_length{0};
  std::string etag;
  std::string version;
};

class IBackend {
 public:
  virtual ~IBackend() = default;
  virtual std::string_view kind() const = 0;
  virtual Result<ObjectMeta> Head(std::string_view bucket, std::string_view key) = 0;
  virtual Result<std::size_t> Read(std::string_view bucket, std::string_view key,
                                    std::uint64_t offset, std::span<std::byte> dst) = 0;
  virtual Result<ObjectMeta> Write(std::string_view bucket, std::string_view key,
                                    std::span<const std::byte> src) = 0;
};
```

### 6.7 `transports/rdma/rdma_engine.h`(私有)

```cpp
struct RdmaConnectionHandle { uint64_t id{0}; };

class RdmaEngine {
 public:
  RdmaEngine(std::string bind_host, int port, std::shared_ptr<spdlog::logger> log);
  ~RdmaEngine();
  Result<bool>                Start();
  void                         Stop();
  bool                         available() const;

  Result<std::unordered_map<std::string, std::string>>
                               PrepareSession(const Session& s);  // 返回 addr/rkey/lkey
  Result<bool>                 Write(RdmaConnectionHandle, std::span<const std::byte>,
                                      uint64_t remote_addr, uint32_t rkey);
  Result<bool>                 Read (RdmaConnectionHandle, std::span<std::byte>,
                                      uint64_t remote_addr, uint32_t rkey);
};
```

> 这些都是骨架,实现细节(线程、CQ、QP 池)在 M2 落地时确定;接口刻意保留扩展空间。

---

## 7. 线程模型

刻意最简:

```
main             ─ 启动/停机,brpc::Server::RunUntilAskedToQuit
brpc workers     ─ num_threads(默认 4),bthread 共享池;HTTP + protobuf service 共用
rdma cm thread   ─ 1 个,rdma_cm event_channel epoll
rdma cq thread   ─ 1 个,busy poll + comp_channel 兜底(M2 启用)
spdlog 后台      ─ async sink 自带 1 线程
```

总线程数(M1) ≈ `num_threads + 2`(brpc workers + spdlog + main)。
启用 RDMA(M2)+ 2 个。

---

## 8. 数据流(2 个核心场景)

### 8.1 HTTP GET 流程

```
brpc http_master.default_method
 → HttpFrontend::Handle
 → 解析 path + Range
 → 不需要 session 时 → TransferEngine::HttpGet(b, k, range, controller)
 → backend.Read(b, k, off, lease) → controller.response_attachment.append
 → 200 + Content-Length / ETag
```

### 8.2 GDS / native-rdma GET 协商 + 搬运

```
[控制面]  brpc ControlPlaneService::OpenSession
 → SessionStore.Create
 → 若 native-rdma:RdmaEngine.PrepareSession 拿 addr/rkey/lkey
 → 返回 OpenSessionResponse

[数据面]  HTTP GET 带 x-fa-session-id + x-amz-rdma-token
 → SessionStore.Find / Claim
 → TransferEngine::RdmaGet(session, target):
     backend.Read 到 staging buffer
     RdmaEngine.Write(handle, staging, remote_addr, rkey)
 → 200 + Content-Length: 0
```

gds-cuobject 路径暂时复用现有 cuObjServer 封装(M2 接通),与 native-rdma 共享 `RdmaEngine::PrepareSession` 的参数协议。

---

## 9. 优雅停机

```
SIGTERM
 → brpc::Server::Stop(grace_ms) → 拒新请求 + 等飞行
 → SessionStore.Cancel 所有 active(M3 完善)
 → RdmaEngine.Stop():active QP 断开 → destroy_qp → dereg_mr → dealloc_pd
 → Backend 析构
 → exit
```

总 deadline 30s,超时直接 abort + dump。

---

## 10. 里程碑(3 个,每个 ~ 1 周)

| # | 范围 | DoD |
|---|---|---|
| **M1 协议骨架** | `infra/` + `backend/` + `SessionStore` + brpc Server 起来 + `ControlPlaneService.OpenSession/Head` + `HttpFrontend` GET/PUT/HEAD 走 `MemoryBackend` | client 跑 HTTP 通路 PUT→GET 端到端通过;`/healthz` + brpc `/vars` 可访问;单测 ≥ 20 个 |
| **M2 RDMA + GDS** | `RdmaEngine`(原生 verbs)+ `transports/gds/cuobj_session`(复用旧 cuObjServer 兼容层)+ `TransferEngine::RdmaGet/Put` | client 跑 native-rdma + gds-cuobject 端到端通过;RDMA 启动失败时降级 HTTP-only |
| **M3 收尾** | 优雅停机完善 + e2e_test + bench(简单 wrk + bvar 截图) + README + 部署示例 | 24h soak 不漏内存;client 全部测例通过;文档完整 |

每个里程碑结束 = 自己跑通 + 评审 + 入主分支。

---

## 11. 与旧实现的关系

- **FusionAccess/gateway** 完全不复用代码,但**复用 proto 文件**(顶层 CMake 已 generate),保持协议字段兼容,client 不需要改协议。
- proto 在评估后期可考虑迁移到 `Us3TurboAccess/gateway/proto/`(M3 可选)。

---

## 12. v2 演进点(显式列出)

第一版接口稳定后再做:

| 项 | 触发条件 |
|---|---|
| Session 分片去全局锁 | session 并发 > 几万,锁等待可观测时 |
| Ticket HMAC / TTL 收割 | 安全审计或长跑 session 泄漏时 |
| BufferPool / MR Pool 预注册 | RDMA 大对象吞吐瓶颈 |
| QP Pool 复用 | RDMA 建链频率瓶颈 |
| 多 worker / thread-per-core | 单机 QPS 瓶颈 |
| nvidia-peermem 直注册 GPU | 替换 cuObjServer host buffer 中转 |
| Admin endpoint(quiesce / drop-sessions / inventory) | 运维需求 |
| SyntheticBackend / FaultyBackend | 大对象正确性 + 故障注入测试 |
| Prometheus exporter(从 bvar 桥接) | 接入 Prometheus 体系时 |
| OpenTelemetry trace | 跨服务 tracing 需求 |

**所有 v2 项都不动 v1 的外部接口**(brpc proto + HTTP 协议)、`IBackend`、`SessionStore`、`RdmaEngine` 公共方法。

---

## 13. 待你确认

确认下列 6 项后即可开工 M1:

1. **模块划分是否 OK**:`core/{server,session,transfer,common}` + `api/` + `transports/{rdma,gds}` + `backend/` + `infra/`?
2. **brpc 单端口承载 HTTP + protobuf** 还是分两端口?(client 现状是单端口 `http_master + ControlPlaneService` 共用,延续此约定)
3. **是否在 v1 就把 gds-cuobject 路径接通**(M2),还是 M1 就跑通?倾向 M2,M1 先 HTTP 通路即可。
4. **`ErrorCode`** 是和 client 共享同一头(放 `Us3TurboAccess/` 顶层 include),还是 gateway 自己声明同值?倾向后者(避免反向依赖)。
5. **proto 文件位置**:M1 继续指向 `FusionAccess/gateway/proto/control_plane.proto`,M3 是否搬到 `Us3TurboAccess/gateway/proto/`?
6. **RDMA 启动失败时**:降级 HTTP-only 启动,还是 fail-fast 退出?倾向降级(便于无 HCA 环境调试)。

---

## 14. 变更记录

| 版本 | 日期 | 摘要 |
|---|---|---|
| v1.0 | 2026-05-18 | 首版重写计划,锁定 brpc 栈、模块划分、目录结构、三方库 |
