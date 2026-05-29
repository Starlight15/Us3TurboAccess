# PUT+HTTP 链路修复进度报告

| 字段 | 值 |
|------|-----|
| 开始日期 | 2026-05-29 |
| 当前阶段 | 阶段 3 进行中 |
| 总进度 | 16/38 (42%) |
| 最新 commit | 3345900 |

---

## ✅ 已完成（16/38 = 42%）

### 阶段 1：提交 P0 #2 + Phase 0（1.9d）✅

| # | 任务 | 完成 | Commit |
|---|------|------|--------|
| 1 | 提交 P0 #2 | ✅ | b14e40f |
| 2 | request_id 生成 | ✅ | 3028412 |
| 3 | access log 结束行 | ✅ | 3028412 |
| 4 | 405 规范 | ✅ | 3028412 |
| 5 | 编译检查 | ✅ | 3028412 |

### 阶段 2：完成 P0 剩余（2.5d）✅

| # | 任务 | 完成 | Commit |
|---|------|------|--------|
| 1 | 并发限制 | ✅ | 769eba0 |
| 2 | bvar 验证 | ✅ | c785258 |
| 3 | CRC 验证 | ✅ | c785258 |
| 4 | Backend 写入验证 | ✅ | c785258 |

### 阶段 3：P1.1 HTTP 零拷贝（10d）🔄 进行中

| # | 任务 | 完成 | Commit | 说明 |
|---|------|------|--------|------|
| 1 | B1-Gateway (阶段1) | ✅ | 3345900 | IOBuf 直通 HttpFrontend → HttpExecutor |
| 2 | B1-Gateway (阶段2) | ❌ | - | Backend 接口支持 IOBuf |
| 3 | B2-Gateway | ❌ | - | CRC 单遍优化 |
| 4 | Stream API | ❌ | - | PutObjectStream(Reader&) |
| 5 | 自动 multipart | ❌ | - | auto_multipart_threshold |

---

## 🔄 进行中（1 项）

### B1-Gateway IOBuf 直通（阶段 1 完成）

**已完成：**
- ✅ HttpFrontend → HttpExecutor 零拷贝
- ✅ 去掉 HandlePut 的 to_string()
- ✅ 去掉 HandleUploadPart 的 to_string()
- ✅ IOBuf 版本的 CRC 计算

**待完成（阶段 2）：**
- ❌ Backend 接口支持 IOBuf
- ❌ 去掉 HttpExecutor 内部的 to_string()

**当前瓶颈：**
```cpp
// HttpExecutor::Put (IOBuf 版本)
const auto payload = body.to_string();  // 仍需拷贝
std::span<const std::byte> span(...);
backend_.Write(bucket, key, span);
```

---

## ⏳ 待完成（22/38 = 58%）

### 阶段 3 剩余任务

**B1-Gateway 阶段 2（2d）：Backend IOBuf 支持**
```
□ 修改 IBackend::Write 接受 IOBuf
□ 修改 IBackend::WritePart 接受 IOBuf
□ 更新 MemoryDataStore 实现
□ 更新 CompositeBackend 实现
□ 去掉 HttpExecutor 内部 to_string()
```

**B2-Gateway（2d）：CRC 单遍优化**
```
□ 边遍历 IOBuf block 边算 CRC
□ 边算边写 backend（single pass）
□ 消除重复遍历
```

**Stream API（3d）**
```
□ 补 PutObjectStream(Reader&) 接口
□ client 端实现
```

**自动 multipart（2d）**
```
□ auto_multipart_threshold (16 MiB)
```

---

## 📊 进度统计

| 优先级 | 总数 | 已完成 | 待修复 | 完成率 |
|--------|------|--------|--------|--------|
| **P0** | 10 | 10 | 0 | **100%** ✅ |
| **P1** | 16 | 4 | 12 | 25% |
| **P2** | 12 | 2 | 10 | 17% |
| **总计** | **38** | **16** | **22** | **42%** |

---

## 🎯 下一步行动

### 立即行动：B1-Gateway 阶段 2（2d）

**目标：Backend 接口支持 IOBuf，彻底消除拷贝**

**步骤：**
1. 修改 `backend/backend.h` 接口
   ```cpp
   // 添加 IOBuf 重载
   Result<ObjectMeta> Write(bucket, key, const butil::IOBuf& body);
   Result<std::string> WritePart(upload_id, part_number, offset, const butil::IOBuf& body);
   ```

2. 实现 MemoryDataStore IOBuf 支持
   ```cpp
   // 遍历 IOBuf block 写入
   for (block : body.backing_blocks()) {
     memcpy(dest, block.data(), block.size());
     dest += block.size();
   }
   ```

3. 更新 HttpExecutor
   ```cpp
   // 去掉 to_string()
   auto write = backend_.Write(bucket, key, body);  // 直接传 IOBuf
   ```

---

## 📝 关键改动

### Commit 3345900: IOBuf 零拷贝（阶段 1）

**优化前：**
```cpp
const auto payload = cntl->request_attachment().to_string();  // 拷贝
const std::span<const std::byte> body(...);
http_.Put(bucket, key, body, crc);
```

**优化后：**
```cpp
const auto& body = cntl->request_attachment();  // 零拷贝
http_.Put(bucket, key, body, crc);
```

**性能提升（预期）：**
- 吞吐：970 MiB/s → ~1.2 GB/s (1.2×)
- 延迟：264 ms → ~200 ms (24% ↓)
- CPU：55% → ~45% (18% ↓)

**剩余瓶颈：**
- HttpExecutor 内部仍需 to_string() 给 backend
- 需要阶段 2 完成彻底消除

---

## 🎉 里程碑

### ✅ P0 完成（100%）
### 🔄 P1.1 零拷贝（40% - 阶段 1 完成）

**下一个里程碑：P1.1 完成（预计 1.5× 性能提升）**

---

## 变更记录

| 版本 | 日期 | 摘要 |
|------|------|------|
| v3.0 | 2026-05-29 | 阶段 3 启动，IOBuf 零拷贝阶段 1 完成 |
| v2.0 | 2026-05-29 | 阶段 2 完成，P0 100% 完成 |
| v1.0 | 2026-05-29 | 阶段 1 完成，11/38 问题已修复 |
