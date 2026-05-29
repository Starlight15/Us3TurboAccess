# PUT+HTTP 完整链路 Review 计划

| 字段 | 值 |
|------|-----|
| 链路类型 | PUT + HTTP (client → gateway) |
| Review 范围 | Client 端 + Gateway 端完整数据流 |
| 目标 | 端到端正确性、性能、可观测性、错误处理 |
| 日期 | 2026-05-29 |

---

## 一、链路概览

### 1.1 完整数据流

```
┌─────────────────────── Client 进程 ───────────────────────┐
│                                                            │
│  Client::PutObject(request, ConstBufferView buffer)       │ ← 用户入口
│    ↓                                                       │
│  TransferRouter::PutObject                                │ ← 路由选择
│    ↓                                                       │
│  HttpTransferPath::PutObject                              │ ← HTTP 通路
│    ├─ Preflight (path + buffer_type 检查)                 │
│    ├─ Crc32c(buffer) (send_crc32c=true 时)                │
│    └─ HttpDataClient::PutObject                           │
│         ↓                                                  │
│       RetryIfRetryable(MakeRetryPolicy, PutObjectOnce)    │ ← 重试逻辑
│         ↓                                                  │
│       PutObjectOnce                                        │
│         ├─ BuildObjectUri(/v1/objects/...)                │
│         ├─ cntl.http_request().set_method(PUT)            │
│         ├─ ApplyRequestHeaders (client_id/bearer)         │
│         ├─ SetHeader x-amz-checksum-crc32c                │
│         ├─ request_attachment().append_user_data          │ ← 零拷贝
│         └─ channel_.channel()->CallMethod(...)            │ ← 同步 RPC
│                                                            │
└────────────────────────────────────────────────────────────┘
                            │
                            │ TCP / HTTP/1.1 keep-alive
                            ↓
┌─────────────────────── Gateway 进程 ──────────────────────┐
│                                                            │
│  HttpFrontend::default_method                             │ ← brpc 入口
│    ├─ logger->info(method path remote_side)               │
│    ├─ 路由：/v1/objects/{bucket}/{key}                    │
│    └─ ParseObjectPath → HandlePut                         │
│         ├─ cntl->request_attachment().to_string()         │ ← ★ 内存拷贝
│         ├─ ParseCrc32cHeader → expected_crc               │
│         ├─ HttpExecutor::Put(bucket, key, body, crc)      │
│         │    ├─ VerifyCrc32c(body, expected)              │ ← 全量算 CRC
│         │    └─ backend_.Write(bucket, key, body)         │
│         │         └─ CompositeBackend::Write              │
│         │              ├─ MemoryDataStore::WriteRange     │ ← memcpy
│         │              └─ index_->PutObjectIndex          │
│         └─ 写响应：200, ETag, x-amz-checksum-crc32c       │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

### 1.2 关键特征

| 维度 | 特征 |
|------|------|
| 协议 | HTTP/1.1 + keep-alive |
| 传输 | 同步阻塞（client 等待 gateway 响应）|
| 零拷贝 | client 端零拷贝（append_user_data），gateway 端有拷贝（to_string）|
| CRC 校验 | 双向（client 算 → gateway 验证 → gateway 算 → client 验证）|
| 重试 | client 端重试（5xx + retryable）|
| 限流 | 无（待补充）|
| 可观测性 | 部分（bvar 已添加，request_id 缺失）|

---

## 二、Review 计划

### 2.1 Review 维度（8 个）

| 维度 | 关注点 | 优先级 |
|------|--------|--------|
| **A. 正确性** | 数据完整性、CRC 校验、错误传播 | P0 |
| **B. 性能** | 内存拷贝、CPU 占用、吞吐延迟 | P0 |
| **C. 错误处理** | 错误码映射、重试逻辑、降级 | P1 |
| **D. 可观测性** | 日志、指标、request_id | P1 |
| **E. 资源管理** | 内存泄漏、连接池、超时 | P1 |
| **F. 安全性** | 鉴权、限流、DoS 防护 | P1 |
| **G. 协议合规** | HTTP 规范、S3 兼容性 | P2 |
| **H. 代码质量** | 命名、注释、测试覆盖 | P2 |

### 2.2 Review 阶段（3 个）

| 阶段 | 范围 | 输出 |
|------|------|------|
| **Phase 1: 静态分析** | 代码阅读 + 架构分析 | Finding 清单 |
| **Phase 2: 动态验证** | 功能测试 + 性能测试 | 测试报告 |
| **Phase 3: 总结归档** | 优化建议 + 文档化 | Review 报告 |

---

## 三、Phase 1: 静态分析（代码阅读）

### 3.1 Client 端 Review 清单

#### A. 正确性（Client）

| # | 检查项 | 文件位置 | 关注点 |
|---|--------|---------|--------|
| A1 | PutObject 入口参数校验 | client.cpp | bucket/key 合法性、buffer 非空 |
| A2 | TransferRouter 路由逻辑 | transfer_router.cpp | HTTP 路径选择条件 |
| A3 | CRC32C 计算正确性 | http_crc32c.cpp | 算法实现、边界条件 |
| A4 | 零拷贝实现 | http_data_client.cpp | append_user_data + deleter |
| A5 | 响应 CRC 验证 | http_data_client.cpp | server_crc32c 校验逻辑 |

#### B. 性能（Client）

| # | 检查项 | 文件位置 | 关注点 |
|---|--------|---------|--------|
| B1 | CRC 计算开销 | http_transfer_path.cpp | 全量 buffer 遍历 |
| B2 | 同步阻塞影响 | http_data_client.cpp | CallMethod 阻塞时长 |
| B3 | 连接池效率 | http_channel.cpp | connection_type=pooled |
| B4 | 超时配置 | http_channel.cpp | timeout_ms 合理性 |

#### C. 错误处理（Client）

| # | 检查项 | 文件位置 | 关注点 |
|---|--------|---------|--------|
| C1 | HTTP 状态码映射 | http_data_client.cpp | HttpStatusToCode 完整性 |
| C2 | 重试策略 | http_retry.h | 重试条件、退避算法 |
| C3 | 错误传播 | client.cpp | Result<T> 错误链 |
| C4 | 超时处理 | http_data_client.cpp | timeout 错误码 |

#### D. 可观测性（Client）

| # | 检查项 | 文件位置 | 关注点 |
|---|--------|---------|--------|
| D1 | 日志完整性 | http_data_client.cpp | 请求开始/结束/失败 |
| D2 | request_id 生成 | - | 是否生成并传递 |
| D3 | 重试次数记录 | http_retry.h | 重试计数器 |

---

### 3.2 Gateway 端 Review 清单

#### A. 正确性（Gateway）

| # | 检查项 | 文件位置 | 关注点 |
|---|--------|---------|--------|
| A1 | 路由解析 | http_frontend.cpp | ParseObjectPath 正确性 |
| A2 | CRC 验证 | http_executor.cpp | VerifyCrc32c 实现 |
| A3 | Backend 写入 | memory_data_store.cpp | WriteRange 原子性 |
| A4 | ETag 生成 | composite_backend.cpp | MakeMeta 幂等性 |
| A5 | 响应 CRC 计算 | http_executor.cpp | 返回 server_crc32c |

#### B. 性能（Gateway）

| # | 检查项 | 文件位置 | 关注点 |
|---|--------|---------|--------|
| B1 | to_string() 拷贝 | http_frontend.cpp | ★ 核心瓶颈 |
| B2 | CRC + Write 两遍 | http_executor.cpp | 两次内存遍历 |
| B3 | Backend memcpy | memory_data_store.cpp | WriteRange 拷贝 |
| B4 | brpc worker 阻塞 | http_executor.cpp | 未下沉 io_pool |

#### C. 错误处理（Gateway）

| # | 检查项 | 文件位置 | 关注点 |
|---|--------|---------|--------|
| C1 | 错误码映射 | error.cpp | ErrorCodeToHttpStatus 完整性 |
| C2 | 异常捕获 | http_frontend.cpp | try-catch 覆盖 |
| C3 | 错误响应格式 | http_frontend.cpp | JSON error body |

#### D. 可观测性（Gateway）

| # | 检查项 | 文件位置 | 关注点 |
|---|--------|---------|--------|
| D1 | 入口日志 | http_frontend.cpp | 请求开始行 |
| D2 | 结束日志 | http_frontend.cpp | ★ 缺失 |
| D3 | request_id | http_frontend.cpp | ★ 缺失 |
| D4 | bvar 指标 | metrics.cpp | http_put_total/bytes/latency |

#### E. 资源管理（Gateway）

| # | 检查项 | 文件位置 | 关注点 |
|---|--------|---------|--------|
| E1 | 内存上限 | http_frontend.cpp | ★ 无 body size 检查 |
| E2 | 并发限制 | gateway_runtime.cpp | ★ 无 max_concurrency |
| E3 | 超时保护 | http_executor.cpp | backend 操作超时 |

#### F. 安全性（Gateway）

| # | 检查项 | 文件位置 | 关注点 |
|---|--------|---------|--------|
| F1 | 鉴权 | http_frontend.cpp | ★ 无鉴权 |
| F2 | 限流 | http_frontend.cpp | ★ 无限流 |
| F3 | DoS 防护 | http_frontend.cpp | ★ 无防护 |

---

## 四、Phase 2: 动态验证（测试）

### 4.1 功能测试清单

| # | 测试场景 | 预期结果 | 验证点 |
|---|---------|---------|--------|
| T1 | 正常 PUT（1 KB） | 200 OK + ETag | 数据完整性 |
| T2 | 正常 PUT（16 MiB） | 200 OK + ETag | 大对象完整性 |
| T3 | 正常 PUT（1 GiB） | 200 OK + ETag | 超大对象完整性 |
| T4 | CRC 校验失败 | 400 Bad Request | 错误检测 |
| T5 | 超过 body 上限 | 413 Payload Too Large | 限流生效 |
| T6 | 并发 PUT 同 key | 200 OK | last-writer-wins |
| T7 | 网络中断重试 | 最终成功或失败 | 重试逻辑 |
| T8 | Gateway 重启 | 503 Service Unavailable | 降级处理 |

### 4.2 性能测试清单

| # | 测试场景 | 指标 | 目标 |
|---|---------|------|------|
| P1 | 单线程 PUT（4 MiB × 100） | 吞吐 / 延迟 | 基线 |
| P2 | 4 线程 PUT（4 MiB × 100） | 吞吐 / 延迟 | 线性扩展 |
| P3 | 单线程 PUT（64 MiB × 32） | 吞吐 / 延迟 | 大对象性能 |
| P4 | CPU 占用 | user / sys / idle | < 60% |
| P5 | 内存占用 | RSS / 峰值 | 无泄漏 |
| P6 | 连接复用 | keep-alive 生效 | 减少握手 |

### 4.3 压力测试清单

| # | 测试场景 | 指标 | 目标 |
|---|---------|------|------|
| S1 | 持续 PUT（1h） | 吞吐稳定性 | ±10% |
| S2 | 并发 PUT（100 线程） | 错误率 | < 1% |
| S3 | 内存泄漏检测 | RSS 增长 | < 1% |
| S4 | 限流验证 | 503 返回 | 正确拒绝 |

---

## 五、Phase 3: 总结归档

### 5.1 输出文档

| 文档 | 内容 | 格式 |
|------|------|------|
| **Finding 清单** | 所有发现的问题 | Markdown 表格 |
| **优化建议** | 按优先级排序的改进项 | Phase 分类 |
| **测试报告** | 功能/性能/压力测试结果 | CSV + 图表 |
| **Review 报告** | 完整 review 总结 | Markdown |

### 5.2 Finding 分类

| 严重度 | 定义 | 处理 |
|--------|------|------|
| **P0** | 阻断性问题（DoS / 数据丢失 / 性能瓶颈）| 立即修复 |
| **P1** | 重要问题（功能缺失 / 错误处理不完整）| 2 周内修复 |
| **P2** | 改进问题（可观测性 / 协议合规）| 1 月内修复 |
| **P3** | 文档问题（注释 / 规范）| 随版本修复 |

---

## 六、Review 注意事项

### 6.1 已知问题（来自 put-http.md）

**P0 级别（2 项）：**
1. **C1/D1/H1**: PUT 内存放大（to_string() 两次拷贝）
2. **C5**: 缺 brpc 限流（无 max_concurrency）

**P1 级别（13 项）：**
- 性能：零拷贝、CRC 单遍、io_pool
- 功能：Stream API、自动 multipart、鉴权
- 错误：错误码映射、重试优化

**P2 级别（13 项）：**
- 可观测性：request_id、access log、bvar
- 协议：100-continue、超时分层

**P3 级别（6 项）：**
- 文档：CRC 说明、error body 格式

### 6.2 Review 重点

**Client 端：**
1. ✅ 零拷贝实现（append_user_data）
2. ⚠️ CRC 全量计算（性能影响）
3. ⚠️ 重试策略（5xx 重试代价）
4. ❌ request_id 缺失

**Gateway 端：**
1. ❌ to_string() 内存拷贝（**核心瓶颈**）
2. ❌ CRC + Write 两遍遍历
3. ❌ 无 body size 上限检查（已修复，待提交）
4. ❌ 无并发限制
5. ❌ 无鉴权/限流

### 6.3 对比其他链路

| 特征 | HTTP | RDMA | GDS |
|------|------|------|-----|
| 协议 | HTTP/1.1 | RDMA verbs | cuObject |
| 零拷贝 | 部分（client 端）| 完全 | 完全 |
| CRC 校验 | 双向 | 可选 | 可选 |
| 性能 | 970 MiB/s | 1.5 GB/s | 7.5 GB/s |
| 复杂度 | 低 | 中 | 高 |
| 依赖 | 无 | RDMA 硬件 | GPU + GDS |

**HTTP 链路特点：**
- ✅ 通用性强（无硬件依赖）
- ✅ 协议成熟（HTTP/1.1）
- ✅ 易于调试（抓包 / curl）
- ❌ 性能较低（内存拷贝多）
- ❌ CPU 占用高（55%）

---

## 七、Review 执行计划

### 7.1 时间安排

| 阶段 | 工作量 | 时间 | 负责人 |
|------|--------|------|--------|
| Phase 1: 静态分析 | 2d | W1 | TBD |
| Phase 2: 动态验证 | 2d | W2 | TBD |
| Phase 3: 总结归档 | 1d | W2 | TBD |
| **总计** | **5d** | **2 周** | - |

### 7.2 交付物

| 交付物 | 格式 | 截止时间 |
|--------|------|---------|
| Finding 清单 | Markdown | Phase 1 结束 |
| 测试报告 | CSV + 图表 | Phase 2 结束 |
| Review 报告 | Markdown | Phase 3 结束 |
| 优化计划 | Markdown | Phase 3 结束 |

---

## 八、后续链路 Review 计划

### 8.1 9 条链路优先级

| # | 链路 | 优先级 | 理由 |
|---|------|--------|------|
| 1 | **PUT + HTTP** | P0 | ✅ 已完成（put-http.md）|
| 2 | **GET + HTTP** | P0 | 协议相同，检查清单复用 |
| 3 | **PUT + RDMA** | P1 | 高性能链路，复杂度中等 |
| 4 | **GET + RDMA** | P1 | 与 PUT + RDMA 对称 |
| 5 | **PUT + GDS** | P1 | 最高性能，复杂度最高 |
| 6 | **GET + GDS** | P1 | 与 PUT + GDS 对称 |
| 7 | **Multipart + HTTP** | P2 | 基于 PUT + HTTP |
| 8 | **Multipart + RDMA** | P2 | 基于 PUT + RDMA |
| 9 | **Multipart + GDS** | P2 | 基于 PUT + GDS |

### 8.2 Review 模板复用

**可复用维度：**
- A. 正确性（数据完整性、CRC 校验）
- C. 错误处理（错误码映射、重试逻辑）
- D. 可观测性（日志、指标、request_id）
- E. 资源管理（内存、连接、超时）
- F. 安全性（鉴权、限流、DoS）
- G. 协议合规（HTTP 规范、S3 兼容）
- H. 代码质量（命名、注释、测试）

**链路特定维度：**
- B. 性能（零拷贝、CPU、吞吐）← 每条链路不同
- RDMA 特定：MR 注册、QP 管理、CQ 轮询
- GDS 特定：cuObject、descriptor、GPU 内存

---

## 九、变更记录

| 版本 | 日期 | 摘要 |
|------|------|------|
| v1.0 | 2026-05-29 | 创建 PUT+HTTP 链路 review 计划 |
