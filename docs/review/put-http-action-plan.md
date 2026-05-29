# PUT+HTTP 链路 Review 问题清单与修复计划

| 字段 | 值 |
|------|-----|
| 链路类型 | PUT + HTTP (client → gateway) |
| 基线 | commit `3dd6658` + P0 #2 (未提交) |
| 总问题数 | 38 项（Client 16 + Gateway 22）|
| 已完成 | 7 项（18%）|
| 待修复 | 31 项（82%）|
| 更新日期 | 2026-05-29 |

---

## 📊 完成进度总览

| 优先级 | 总数 | 已完成 | 待修复 | 完成率 |
|--------|------|--------|--------|--------|
| **P0** | 10 | 3 | 7 | 30% |
| **P1** | 16 | 2 | 14 | 13% |
| **P2** | 12 | 2 | 10 | 17% |
| **总计** | **38** | **7** | **31** | **18%** |

---

## 一、Client 端问题清单（16 项）

### A. 正确性（Client）- 5 项

| # | 检查项 | 完成 | 文件位置 | 优先级 | 说明 |
|---|--------|------|---------|--------|------|
| A1 | PutObject 入口参数校验 | ✅ | client.cpp | P0 | bucket/key 合法性已校验 |
| A2 | TransferRouter 路由逻辑 | ✅ | transfer_router.cpp | P0 | HTTP 路径选择正确 |
| A3 | CRC32C 计算正确性 | ✅ | http_crc32c.cpp | P0 | 算法实现正确 |
| A4 | 零拷贝实现 | ✅ | http_data_client.cpp | P0 | append_user_data 正确 |
| A5 | 响应 CRC 验证 | ❌ | http_data_client.cpp | P1 | 需验证 server_crc32c 校验逻辑 |

### B. 性能（Client）- 4 项

| # | 检查项 | 完成 | 文件位置 | 优先级 | 说明 |
|---|--------|------|---------|--------|------|
| B1 | CRC 计算开销 | ❌ | http_transfer_path.cpp | P1 | 全量 buffer 遍历，待优化 |
| B2 | 同步阻塞影响 | ❌ | http_data_client.cpp | P2 | CallMethod 阻塞，待异步化 |
| B3 | 连接池效率 | ✅ | http_channel.cpp | P1 | connection_type=pooled 已设置 |
| B4 | 超时配置 | ❌ | http_channel.cpp | P2 | timeout_ms 未分层 |

### C. 错误处理（Client）- 4 项

| # | 检查项 | 完成 | 文件位置 | 优先级 | 说明 |
|---|--------|------|---------|--------|------|
| C1 | HTTP 状态码映射 | 🟡 | http_data_client.cpp | P1 | 413 已补全，507/409/416/401 待补 |
| C2 | 重试策略 | ❌ | http_retry.h | P1 | 重试条件、退避算法待验证 |
| C3 | 错误传播 | ✅ | client.cpp | P0 | Result<T> 错误链正确 |
| C4 | 超时处理 | ❌ | http_data_client.cpp | P2 | timeout 错误码待验证 |

### D. 可观测性（Client）- 3 项

| # | 检查项 | 完成 | 文件位置 | 优先级 | 说明 |
|---|--------|------|---------|--------|------|
| D1 | 日志完整性 | ❌ | http_data_client.cpp | P1 | 请求开始/结束/失败日志待补全 |
| D2 | request_id 生成 | ❌ | - | P1 | 缺失，待实现 |
| D3 | 重试次数记录 | ❌ | http_retry.h | P2 | 重试计数器待添加 |

---

## 二、Gateway 端问题清单（22 项）

### A. 正确性（Gateway）- 5 项

| # | 检查项 | 完成 | 文件位置 | 优先级 | 说明 |
|---|--------|------|---------|--------|------|
| A1 | 路由解析 | ✅ | http_frontend.cpp | P0 | ParseObjectPath 正确 |
| A2 | CRC 验证 | ❌ | http_executor.cpp | P0 | VerifyCrc32c 实现待验证 |
| A3 | Backend 写入 | ❌ | memory_data_store.cpp | P0 | WriteRange 原子性待验证 |
| A4 | ETag 生成 | ❌ | composite_backend.cpp | P1 | MakeMeta 非幂等（ticks-based）|
| A5 | 响应 CRC 计算 | ❌ | http_executor.cpp | P1 | server_crc32c 返回待验证 |

### B. 性能（Gateway）- 4 项

| # | 检查项 | 完成 | 文件位置 | 优先级 | 说明 |
|---|--------|------|---------|--------|------|
| B1 | to_string() 拷贝 | ❌ | http_frontend.cpp:436/515/540 | **P0** | ★ 核心瓶颈，3 处待去除 |
| B2 | CRC + Write 两遍 | ❌ | http_executor.cpp | **P0** | 两次内存遍历待优化 |
| B3 | Backend memcpy | ❌ | memory_data_store.cpp | P1 | WriteRange 拷贝待优化 |
| B4 | brpc worker 阻塞 | ❌ | http_executor.cpp | P1 | 未下沉 io_pool |

### C. 错误处理（Gateway）- 3 项

| # | 检查项 | 完成 | 文件位置 | 优先级 | 说明 |
|---|--------|------|---------|--------|------|
| C1 | 错误码映射 | 🟡 | error.cpp | P1 | 部分完成，待补全 |
| C2 | 异常捕获 | ❌ | http_frontend.cpp | P2 | try-catch 覆盖待验证 |
| C3 | 错误响应格式 | ❌ | http_frontend.cpp | P2 | JSON error body 待规范 |

### D. 可观测性（Gateway）- 4 项

| # | 检查项 | 完成 | 文件位置 | 优先级 | 说明 |
|---|--------|------|---------|--------|------|
| D1 | 入口日志 | ✅ | http_frontend.cpp:227 | P1 | 请求开始行已有 |
| D2 | 结束日志 | ❌ | http_frontend.cpp | **P1** | ★ 缺失，待补充 |
| D3 | request_id | ❌ | http_frontend.cpp | **P1** | ★ 缺失，待实现 |
| D4 | bvar 指标 | 🟡 | metrics.cpp | P0 | 已添加，待验证上报 |

### E. 资源管理（Gateway）- 3 项

| # | 检查项 | 完成 | 文件位置 | 优先级 | 说明 |
|---|--------|------|---------|--------|------|
| E1 | 内存上限 | 🟡 | http_frontend.cpp | **P0** | 已实现（P0 #2），待提交 |
| E2 | 并发限制 | ❌ | gateway_runtime.cpp | **P0** | ★ 无 max_concurrency |
| E3 | 超时保护 | ❌ | http_executor.cpp | P1 | backend 操作超时待添加 |

### F. 安全性（Gateway）- 3 项

| # | 检查项 | 完成 | 文件位置 | 优先级 | 说明 |
|---|--------|------|---------|--------|------|
| F1 | 鉴权 | ❌ | http_frontend.cpp | P1 | ★ 无鉴权，待实现 |
| F2 | 限流 | ❌ | http_frontend.cpp | P1 | ★ 无限流，待实现 |
| F3 | DoS 防护 | ❌ | http_frontend.cpp | P1 | ★ 无防护，待实现 |

---

## 三、按优先级的修复计划

### 🔴 P0: 阻断性问题（10 项，7 项待修复）

#### 已完成（3 项）

| # | 问题 | 完成 | 说明 |
|---|------|------|------|
| A1-Client | PutObject 参数校验 | ✅ | 已实现 |
| A2-Client | TransferRouter 路由 | ✅ | 已实现 |
| A3-Client | CRC32C 计算 | ✅ | 已实现 |

#### 待修复（7 项）

| # | 问题 | 文件位置 | 工作量 | 说明 |
|---|------|---------|--------|------|
| **1. B1-Gateway** | **to_string() 拷贝** | http_frontend.cpp:436/515/540 | 3d | ★ 核心瓶颈，去掉 3 处 |
| **2. B2-Gateway** | **CRC + Write 两遍** | http_executor.cpp | 2d | ★ single pass 优化 |
| **3. E2-Gateway** | **并发限制** | gateway_runtime.cpp | 1d | 设置 max_concurrency |
| **4. D4-Gateway** | **bvar 验证** | metrics.cpp | 0.5d | 验证上报正确性 |
| **5. E1-Gateway** | **内存上限** | - | 0.5d | 提交 P0 #2 |
| **6. A2-Gateway** | **CRC 验证** | http_executor.cpp | 0.5d | 验证 VerifyCrc32c |
| **7. A3-Gateway** | **Backend 写入** | memory_data_store.cpp | 0.5d | 验证 WriteRange 原子性 |

**P0 总工作量：8d**

---

### 🟡 P1: 重要问题（16 项，14 项待修复）

#### 已完成（2 项）

| # | 问题 | 完成 | 说明 |
|---|------|------|------|
| B3-Client | 连接池效率 | ✅ | connection_type=pooled |
| D1-Gateway | 入口日志 | ✅ | 请求开始行已有 |

#### 待修复（14 项）

| # | 问题 | 文件位置 | 工作量 | 说明 |
|---|------|---------|--------|------|
| **1. D2-Gateway** | **结束日志** | http_frontend.cpp | 0.5d | access log 结束行 |
| **2. D3-Gateway** | **request_id** | http_frontend.cpp | 0.5d | 生成 + 回写响应头 |
| **3. C1-Client** | **错误码映射** | http_data_client.cpp | 0.5d | 补全 507/409/416/401 |
| **4. C1-Gateway** | **错误码映射** | error.cpp | 0.5d | 补全映射 |
| **5. B1-Client** | **CRC 计算开销** | http_transfer_path.cpp | 2d | 优化或文档化 |
| **6. B3-Gateway** | **Backend memcpy** | memory_data_store.cpp | 2d | IOBuf 接口 |
| **7. B4-Gateway** | **brpc worker 阻塞** | http_executor.cpp | 2d | 下沉 io_pool |
| **8. A4-Gateway** | **ETag 生成** | composite_backend.cpp | 2d | 改为内容 hash |
| **9. A5-Client** | **响应 CRC 验证** | http_data_client.cpp | 0.5d | 验证逻辑 |
| **10. A5-Gateway** | **响应 CRC 计算** | http_executor.cpp | 0.5d | 验证返回 |
| **11. C2-Client** | **重试策略** | http_retry.h | 1d | 验证 + 优化 |
| **12. D1-Client** | **日志完整性** | http_data_client.cpp | 0.5d | 补全日志 |
| **13. D2-Client** | **request_id 生成** | - | 0.5d | client 端生成 |
| **14. E3-Gateway** | **超时保护** | http_executor.cpp | 1d | backend 超时 |
| **15. F1-Gateway** | **鉴权** | http_frontend.cpp | 5d | sigv4 / bearer |
| **16. F2/F3-Gateway** | **限流 + DoS** | http_frontend.cpp | 2d | 限流 + 防护 |

**P1 总工作量：20.5d**

---

### 🟢 P2: 改进问题（12 项，10 项待修复）

#### 已完成（2 项）

| # | 问题 | 完成 | 说明 |
|---|------|------|------|
| A4-Client | 零拷贝实现 | ✅ | append_user_data |
| C3-Client | 错误传播 | ✅ | Result<T> 正确 |

#### 待修复（10 项）

| # | 问题 | 文件位置 | 工作量 | 说明 |
|---|------|---------|--------|------|
| **1. B2-Client** | **同步阻塞** | http_data_client.cpp | 3d | 异步化 |
| **2. B4-Client** | **超时配置** | http_channel.cpp | 1d | 超时分层 |
| **3. C4-Client** | **超时处理** | http_data_client.cpp | 0.5d | 验证错误码 |
| **4. D3-Client** | **重试计数器** | http_retry.h | 0.5d | 添加 bvar |
| **5. C2-Gateway** | **异常捕获** | http_frontend.cpp | 1d | try-catch 覆盖 |
| **6. C3-Gateway** | **错误响应格式** | http_frontend.cpp | 0.5d | JSON 规范化 |

**P2 总工作量：6.5d**

---

## 四、分阶段修复计划

### 阶段 1：提交 P0 #2 + Phase 0（本周，1.9d）

| # | 任务 | 完成 | 工作量 | 说明 |
|---|------|------|--------|------|
| 1 | 提交 P0 #2 | ❌ | 0.5d | E1-Gateway: 内存上限 |
| 2 | D2-Gateway | ❌ | 0.5d | 结束日志 |
| 3 | D3-Gateway | ❌ | 0.5d | request_id |
| 4 | 编译检查 | ❌ | 0.1d | error.cpp switch |
| 5 | 405 规范 | ❌ | 0.1d | Allow 头 |

**验收标准：**
- ✅ P0 #2 提交
- ✅ access log 完整
- ✅ request_id 串联

---

### 阶段 2：完成 P0 剩余（下周，2.5d）

| # | 任务 | 完成 | 工作量 | 说明 |
|---|------|------|--------|------|
| 1 | E2-Gateway | ❌ | 1d | 并发限制 |
| 2 | D4-Gateway | ❌ | 0.5d | bvar 验证 |
| 3 | A2-Gateway | ❌ | 0.5d | CRC 验证 |
| 4 | A3-Gateway | ❌ | 0.5d | Backend 写入验证 |

**验收标准：**
- ✅ brpc 限流生效
- ✅ bvar 准确上报
- ✅ CRC 校验正确

---

### 阶段 3：P1.1 HTTP 零拷贝（两周后，10d）

| # | 任务 | 完成 | 工作量 | 说明 |
|---|------|------|--------|------|
| 1 | B1-Gateway | ❌ | 3d | IOBuf 直通（去掉 to_string）|
| 2 | B2-Gateway | ❌ | 2d | CRC 单遍优化 |
| 3 | Stream API | ❌ | 3d | PutObjectStream(Reader&) |
| 4 | 自动 multipart | ❌ | 2d | auto_multipart_threshold |

**性能目标：**
- 吞吐：970 MiB/s → 1.5 GB/s（1.5×）
- 延迟：264 ms → < 180 ms
- CPU：55% → < 40%

---

### 阶段 4：P1 其他问题（2 周，10.5d）

| # | 任务 | 完成 | 工作量 | 说明 |
|---|------|------|--------|------|
| 1 | C1-Client/Gateway | ❌ | 1d | 错误码映射补全 |
| 2 | D1/D2-Client | ❌ | 1d | 日志 + request_id |
| 3 | B3-Gateway | ❌ | 2d | Backend IOBuf 接口 |
| 4 | B4-Gateway | ❌ | 2d | 下沉 io_pool |
| 5 | A4-Gateway | ❌ | 2d | ETag 内容 hash |
| 6 | 其他验证 | ❌ | 2.5d | A5/C2/E3 验证 |

---

### 阶段 5：P1 安全性（2 周，7d）

| # | 任务 | 完成 | 工作量 | 说明 |
|---|------|------|--------|------|
| 1 | F1-Gateway | ❌ | 5d | 鉴权（sigv4 / bearer）|
| 2 | F2/F3-Gateway | ❌ | 2d | 限流 + DoS 防护 |

---

### 阶段 6：P2 改进（1 周，6.5d）

| # | 任务 | 完成 | 工作量 | 说明 |
|---|------|------|--------|------|
| 1 | B2-Client | ❌ | 3d | 异步化 |
| 2 | B4-Client | ❌ | 1d | 超时分层 |
| 3 | 其他 | ❌ | 2.5d | C4/D3/C2/C3 |

---

## 五、时间线总览

| 周次 | 阶段 | 任务数 | 工作量 | 关键交付 |
|------|------|--------|--------|---------|
| W1 | 提交 P0 #2 + Phase 0 | 5 | 1.9d | 内存上限 + request_id + access log |
| W2 | 完成 P0 剩余 | 4 | 2.5d | 并发限制 + bvar 验证 |
| W3-W4 | P1.1 零拷贝 | 4 | 10d | **IOBuf 直通（1.5× 性能）** |
| W5-W6 | P1 其他 | 6 | 10.5d | 错误码 + io_pool + ETag |
| W7-W8 | P1 安全性 | 2 | 7d | 鉴权 + 限流 |
| W9 | P2 改进 | 4 | 6.5d | 异步化 + 超时分层 |

**总计：9 周，38.4 人天**

---

## 六、验收标准

### P0 验收标准
- ✅ 无内存拷贝瓶颈（去掉 to_string）
- ✅ CRC + Write single pass
- ✅ brpc 限流生效
- ✅ bvar 准确上报
- ✅ 内存上限保护

### P1 验收标准
- ✅ 性能目标达成（1.5× 吞吐）
- ✅ request_id 端到端串联
- ✅ 错误码双向映射对称
- ✅ 鉴权 + 限流生效

### P2 验收标准
- ✅ 异步化完成
- ✅ 超时分层配置
- ✅ 日志完整规范

---

## 变更记录

| 版本 | 日期 | 摘要 |
|------|------|------|
| v1.0 | 2026-05-29 | 创建问题清单与修复计划 |
