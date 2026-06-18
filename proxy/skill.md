# Proxy 代码框架规范

## 一、设计原则

### 1.1 核心定位
- **控制面专职**：只负责 session、路由、元数据，不处理字节流
- **3 链路隔离**：HTTP / UCX / GDS 独立模块，互不依赖
- **无状态优先**：session 由 client 持有 ticket，proxy 只做校验（v1 可内存 map 简化）
- **可测试性**：核心逻辑与 brpc 解耦，可单元测试

### 1.2 代码质量标准
- **职责清晰**：一个类只做一件事，文件名即职责
- **不过度抽象**：v1 不预设 interface/策略模式，等需求明确再抽象
- **不冗余**：3 链路共性逻辑提取到 `common/`，但不强制统一
- **注释规范**：类级说明职责和依赖，函数说明前置条件和返回值语义

---

## 二、目录结构

```
proxy/
├── CMakeLists.txt
├── skill.md                    # 本文档
├── src/
│   ├── main.cpp                # 入口：启动 brpc server
│   │
│   ├── service/                # RPC 服务层（薄适配层）
│   │   ├── proxy_control_plane_service.h
│   │   └── proxy_control_plane_service.cpp
│   │
│   ├── session/                # Session 管理
│   │   ├── session_manager.h
│   │   └── session_manager.cpp
│   │
│   ├── routing/                # Backend 路由
│   │   ├── backend_router.h
│   │   └── backend_router.cpp
│   │
│   ├── handlers/               # 3 链路隔离的上传处理器
│   │   ├── http_upload_handler.h
│   │   ├── http_upload_handler.cpp
│   │   ├── ucx_upload_handler.h
│   │   ├── ucx_upload_handler.cpp
│   │   ├── gds_upload_handler.h
│   │   └── gds_upload_handler.cpp
│   │
│   ├── common/                 # 通用工具
│   │   ├── errors.h            # 统一错误码
│   │   ├── config.h            # 配置结构体
│   │   └── config.cpp
│   │
│   └── utils/                  # 纯工具函数（无状态）
│       ├── id_generator.h      # session_id / upload_id 生成
│       └── id_generator.cpp
│
└── tests/                      # 单元测试（后续补充）
    ├── session_manager_test.cpp
    └── backend_router_test.cpp
```

---

## 三、模块职责

### 3.1 `src/main.cpp`
**职责**：
- 解析命令行参数（gflags）
- 初始化全局单例（SessionManager、BackendRouter）
- 启动 brpc server
- 信号处理和优雅退出

**规范**：
- 不包含业务逻辑
- 所有配置通过 `common/config.h` 统一管理
- 启动顺序：先初始化依赖（router、session_manager），再启动 server

---

### 3.2 `service/proxy_control_plane_service.{h,cpp}`
**职责**：
- 实现 `ControlPlaneService` protobuf 接口
- **薄适配层**：只做参数校验 + 调用核心模块 + 返回 response
- 不包含业务逻辑（逻辑在 handlers/ 或 session/）

**规范**：
```cpp
// 典型实现模式：
void ProxyControlPlaneService::StartUpload(
    RpcController* cntl_base,
    const StartUploadRequest* request,
    StartUploadResponse* response,
    Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);

  // 1. 参数校验（轻量，只检查必填字段）
  if (request->bucket().empty()) {
    cntl->SetFailed(PROXY_ERR_INVALID_PARAM, "missing bucket");
    return;
  }

  // 2. 路由到对应 handler（根据 data_path）
  if (request->data_path() == "gds-cuobject") {
    gds_handler_.StartUpload(request, response, cntl);
  } else {
    cntl->SetFailed(PROXY_ERR_UNSUPPORTED_PATH, "unknown data_path");
  }
}
```

**禁止**：
- ❌ 在此层写 `for` 循环调 backend
- ❌ 在此层写 bthread 并发逻辑
- ❌ 在此层做复杂的状态判断

---

### 3.3 `session/session_manager.{h,cpp}`
**职责**：
- 生成 session_id（格式：`pxs-<seq>`）
- 生成和校验 ticket（v1 简化：base64(session_id + secret)）
- session 过期检查（v1 内存 map，后续可换 Redis）

**接口设计**：
```cpp
class SessionManager {
 public:
  struct Session {
    std::string session_id;
    std::string ticket;
    std::string data_path;      // "gds-cuobject" / "http" / "ucx"
    std::string op_type;        // "PUT" / "GET"
    int64_t     expire_at_sec;  // Unix 时间戳
  };

  // 创建 session（OpenSession 调用）
  Session CreateSession(const std::string& data_path,
                        const std::string& op_type,
                        int ttl_sec);

  // 校验 ticket（StartUpload 等调用）
  // 返回 nullptr 表示无效/过期
  const Session* ValidateTicket(const std::string& ticket);

 private:
  std::atomic<uint64_t> seq_{0};
  std::mutex            mu_;
  std::unordered_map<std::string, Session> sessions_;  // key=session_id
};
```

**规范**：
- 线程安全（`mu_` 保护 `sessions_`）
- v1 不做分布式一致性，单机内存足够
- 过期清理可用 bthread 定时器（每 60s 扫一次）

---

### 3.4 `routing/backend_router.{h,cpp}`
**职责**：
- 维护 backend 列表（v1 静态配置，后续接服务发现）
- 选择可用 backend（v1 轮询，后续可加权重/健康检查）
- 创建到 backend 的 brpc Channel（复用连接池）

**接口设计**：
```cpp
class BackendRouter {
 public:
  struct Backend {
    std::string id;           // "backend-0"
    std::string endpoint;     // "127.0.0.1:9200"
    brpc::Channel* channel;   // 连接池，生命周期由 router 管理
  };

  // 初始化：从配置加载 backend 列表
  bool Init(const std::vector<std::string>& endpoints);

  // 选择一个 backend（轮询 + 跳过失败的）
  Backend* SelectBackend();

  // 标记 backend 失败（供熔断用，v1 可先不实现）
  void MarkFailed(const std::string& backend_id);

 private:
  std::vector<Backend> backends_;
  std::atomic<size_t>  next_index_{0};  // 轮询索引
};
```

**规范**：
- `brpc::Channel` 在 `Init()` 中创建，析构时释放
- `SelectBackend()` 必须线程安全（`atomic` + 轮询）
- v1 不做心跳检测，等 backend 返回错误时再标记失败

---

### 3.5 `handlers/gds_upload_handler.{h,cpp}`
**职责**：
- 实现 GDS 链路的 **StartUpload / GdsPut / CompleteUpload**
- 调用 `backend_router_` 选择 backend
- 使用 bthread 处理与 backend 的 RPC（保持同步写法）

**接口设计**：
```cpp
class GdsUploadHandler {
 public:
  GdsUploadHandler(SessionManager& session_mgr,
                   BackendRouter& backend_router);

  // StartUpload：生成 upload_id，选择 backend
  void StartUpload(const StartUploadRequest* req,
                   StartUploadResponse* resp,
                   brpc::Controller* cntl);

  // CompleteUpload：通知 backend 完成（v1 可 no-op）
  void CompleteUpload(const CompleteUploadRequest* req,
                      CompleteUploadResponse* resp,
                      brpc::Controller* cntl);

 private:
  SessionManager& session_mgr_;
  BackendRouter&  backend_router_;
};
```

**规范**：
- **不直接实现 `GdsPut`**：GdsPut 由 client 直连 backend，proxy 不参与
- `StartUpload` 内部用 bthread 并发调 backend（如果需要多选一）
- 错误处理：backend 失败时返回明确错误码（见 `common/errors.h`）

---

### 3.6 `handlers/http_upload_handler.{h,cpp}` & `handlers/ucx_upload_handler.{h,cpp}`
**职责**：
- 分别实现 HTTP 和 UCX 链路的上传逻辑
- 与 `gds_upload_handler.cpp` **完全隔离**，不共享代码

**规范**：
- v1 可先占位（返回 "not implemented"）
- 未来实现时，即使逻辑相似，也优先**复制代码**而非强行抽象
  - 例如：HTTP 可能需要分段上传状态，GDS 不需要
  - 过早统一接口会导致后续改动牵一发动全身

**目录结构保证隔离**：
```
handlers/
├── gds_upload_handler.cpp    # 只 include session_manager.h + backend_router.h
├── http_upload_handler.cpp   # 只 include session_manager.h（不依赖 backend_router）
└── ucx_upload_handler.cpp    # 同上
```

---

### 3.7 `common/errors.h`
**职责**：
- 定义统一错误码（供 `cntl->SetFailed()` 使用）

**示例**：
```cpp
#pragma once
#include <string_view>

namespace proxy {

// 错误码范围：10000-19999
constexpr int PROXY_ERR_INVALID_PARAM       = 10001;
constexpr int PROXY_ERR_SESSION_EXPIRED     = 10002;
constexpr int PROXY_ERR_BACKEND_UNAVAILABLE = 10003;
constexpr int PROXY_ERR_UNSUPPORTED_PATH    = 10004;

// 错误消息（可选，用于日志）
constexpr std::string_view ErrorMessage(int code) {
  switch (code) {
    case PROXY_ERR_INVALID_PARAM:       return "invalid parameter";
    case PROXY_ERR_SESSION_EXPIRED:     return "session expired";
    case PROXY_ERR_BACKEND_UNAVAILABLE: return "no backend available";
    case PROXY_ERR_UNSUPPORTED_PATH:    return "unsupported data_path";
    default:                            return "unknown error";
  }
}

}  // namespace proxy
```

**规范**：
- 错误码分段：10xxx（参数错误）、11xxx（session 错误）、12xxx（backend 错误）
- 不使用字符串错误码（brpc 支持 int）

---

### 3.8 `common/config.{h,cpp}`
**职责**：
- 封装所有 gflags 配置为结构体
- 提供默认值和校验逻辑

**示例**：
```cpp
#pragma once
#include <string>
#include <vector>

namespace proxy {

struct ProxyConfig {
  int         proxy_port = 9100;
  int         session_ttl_sec = 300;
  std::vector<std::string> backend_endpoints;  // 从 gflags 逗号分隔解析

  // 从 gflags 加载
  static ProxyConfig FromFlags();
};

}  // namespace proxy
```

---

### 3.9 `utils/id_generator.{h,cpp}`
**职责**：
- 生成全局唯一 ID（session_id、upload_id）

**接口**：
```cpp
namespace proxy {

// 生成 session_id："pxs-<timestamp>-<seq>"
std::string GenerateSessionId();

// 生成 upload_id："upl-<timestamp>-<seq>-<random>"
std::string GenerateUploadId();

}  // namespace proxy
```

**规范**：
- 线程安全（内部用 `std::atomic<uint64_t>`）
- ID 格式统一，便于日志追踪

---

## 四、编写规范

### 4.1 链路隔离
**强制规则**：
- `handlers/gds_upload_handler.cpp` **不得** `#include "http_upload_handler.h"`
- 3 个 handler 只依赖 `session/` 和 `routing/`，不互相引用
- 如果有共性代码（如参数校验），优先**复制**到各自文件，而非提取到 `common/`
  - 原因：v1 共性不明确，过早抽象会导致后续改动困难

### 4.2 职责清晰
**文件命名即职责**：
- `session_manager.cpp`：只管 session，不管 backend
- `backend_router.cpp`：只管 backend 选择，不管 session
- `proxy_control_plane_service.cpp`：只做 RPC 适配，不写业务逻辑

**类的职责单一**：
- 一个类不超过 500 行（超过则拆分）
- 公开方法不超过 10 个（超过则职责过多）

### 4.3 注释规范
**类级注释**（必须）：
```cpp
/**
 * @brief 管理 proxy 的 session 生命周期。
 *
 * 职责：
 * - 生成 session_id 和 ticket
 * - 校验 ticket 有效性
 * - 清理过期 session（bthread 定时器）
 *
 * 线程安全：所有方法可并发调用。
 *
 * 依赖：
 * - 无外部依赖（纯内存实现）
 */
class SessionManager { /*...*/ };
```

**函数注释**（简洁）：
```cpp
// 创建新 session，返回 session_id 和 ticket。
// ttl_sec: session 有效期（秒），0 表示使用默认值。
Session CreateSession(const std::string& data_path,
                      const std::string& op_type,
                      int ttl_sec = 0);
```

**行内注释**（关键逻辑）：
```cpp
// 轮询选择 backend，跳过最近失败的（熔断逻辑）
Backend* SelectBackend() {
  for (size_t i = 0; i < backends_.size(); ++i) {
    size_t idx = next_index_.fetch_add(1) % backends_.size();
    if (!backends_[idx].is_failed) {
      return &backends_[idx];  // 找到可用 backend
    }
  }
  return nullptr;  // 所有 backend 都失败
}
```

**禁止**：
- ❌ 复述代码的注释：`// 设置 bucket` → `req->set_bucket(...)`
- ❌ TODO 注释过多（超过 3 个 TODO 说明设计不完整，应重构）

### 4.4 不冗余
**提取共性的时机**：
- ✅ 同一文件内重复 3 次以上 → 提取为 `private` 方法
- ✅ 跨文件重复，且逻辑**稳定不变** → 提取到 `utils/`
- ❌ 跨文件重复，但逻辑**可能分化** → 先保持重复，等需求明确再抽象

**示例**：
```cpp
// ❌ 过早抽象：HTTP / GDS 参数校验逻辑未来可能不同
bool ValidateUploadRequest(const StartUploadRequest* req);  // 强行统一

// ✅ 各自实现：等 3 链路都稳定后再考虑抽象
bool GdsUploadHandler::ValidateRequest(const StartUploadRequest* req) { /*...*/ }
bool HttpUploadHandler::ValidateRequest(const StartUploadRequest* req) { /*...*/ }
```

### 4.5 不过度抽象
**v1 原则**：
- ❌ 不预设 `IUploadHandler` interface（3 链路差异大，强行统一会导致空方法）
- ❌ 不用策略模式选择 handler（直接 `if-else` 判断 `data_path` 足够）
- ✅ 用具体类（`GdsUploadHandler`、`HttpUploadHandler`），等需求明确再重构

**何时抽象**：
- 等 3 个 handler 都实现后，发现**确实有共性**（如日志格式、错误处理）
- 此时再提取 `BaseUploadHandler`，避免现在猜测未来需求

### 4.6 依赖管理
**依赖方向**：
```
main.cpp
  ↓
service/proxy_control_plane_service.cpp
  ↓
handlers/{gds,http,ucx}_upload_handler.cpp
  ↓
session/session_manager.cpp + routing/backend_router.cpp
  ↓
common/{errors,config}.h + utils/id_generator.cpp
```

**禁止**：
- ❌ `session_manager.cpp` 依赖 `backend_router.cpp`（职责交叉）
- ❌ `utils/` 依赖 `handlers/`（工具层不依赖业务层）
- ❌ 循环依赖（编译器会报错，但设计阶段就应避免）

---

## 五、实施步骤

### P0（立即实施）
1. **创建目录结构**：按上述 `src/` 分层创建空文件
2. **实现 `common/errors.h`**：定义错误码
3. **实现 `utils/id_generator.cpp`**：session_id / upload_id 生成
4. **实现 `session/session_manager.cpp`**：内存版 session 管理
5. **实现 `routing/backend_router.cpp`**：轮询选择 backend
6. **重构 `proxy_control_plane_service.cpp`**：
   - 将现有逻辑拆分到 `GdsUploadHandler`
   - service 层只保留参数校验 + handler 调用

### P1（后续优化）
1. 补充单元测试（`tests/`）
2. 实现 HTTP / UCX handler
3. 添加 Prometheus metrics（复用 brpc bvar）

### P2（长期改进）
1. Backend 健康检查和熔断
2. Session 持久化（Redis）
3. 配置热更新

---

## 六、示例：重构后的调用链

**场景**：Client 调用 `StartUpload`（GDS 链路）

```
1. client → proxy:9100/ControlPlaneService.StartUpload

2. ProxyControlPlaneService::StartUpload()
   ├─ 参数校验：bucket/object_key 非空？
   └─ 路由到 gds_handler_.StartUpload()

3. GdsUploadHandler::StartUpload()
   ├─ 校验 ticket：session_mgr_.ValidateTicket()
   ├─ 生成 upload_id：GenerateUploadId()
   ├─ 选择 backend：backend_router_.SelectBackend()
   ├─ 用 bthread 调 backend RPC（同步写法）：
   │  bthread_start_background([&]() {
   │    brpc::Controller cntl;
   │    backend->channel->CallMethod(...);
   │    if (cntl.Failed()) { /* 标记 backend 失败 */ }
   │  });
   └─ 返回 response：backend_id + upload_id

4. client 收到响应，开始 GdsPut 直连 backend
```

**关键点**：
- **service 层**：只做适配，10 行代码
- **handler 层**：业务逻辑，用 bthread 保持同步写法
- **session/routing 层**：纯逻辑，可单元测试（不依赖 brpc）

---

## 七、检查清单

提交代码前，确认：
- [ ] 每个文件职责清晰，文件名即职责
- [ ] 3 链路 handler 无交叉 `#include`
- [ ] `service/` 层无业务逻辑（只有参数校验 + handler 调用）
- [ ] 所有类有类级注释（说明职责和依赖）
- [ ] 无循环依赖（`session/` ↔ `routing/`）
- [ ] 错误处理用统一错误码（`common/errors.h`）
- [ ] 无 TODO 超过 3 个（超过则说明设计不完整）

---

**总结**：本框架以**职责清晰、链路隔离、不过度抽象**为核心，v1 优先实现 GDS 单链路，等需求明确后再统一 3 链路的共性逻辑。
