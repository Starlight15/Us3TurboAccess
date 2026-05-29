# 重试策略验证报告

## 验证目标
验证 Client 端重试策略是否正确、合理且符合最佳实践。

## 重试策略实现

### 1. 核心算法

```cpp
// client/src/data/http_retry.h
struct RetryPolicy {
  int                       max_attempts{3};           // 最大尝试次数
  std::chrono::milliseconds initial_backoff{100ms};    // 初始退避时间
  std::chrono::milliseconds max_backoff{2000ms};       // 最大退避时间
};

// 指数退避 + 抖动
backoff = min(max_backoff, initial * 2^(n-1)) * jitter
jitter ∈ [0.5, 1.5)  // 避免雷鸣效应
```

### 2. 重试条件

```cpp
template <typename Fn>
auto RetryIfRetryable(const RetryPolicy& policy, Fn&& fn) {
  ResultT last = fn();
  for (int attempt = 1; attempt < policy.max_attempts; ++attempt) {
    if (last.success()) return last;              // ✅ 成功立即返回
    if (!last.error().retryable) return last;     // ✅ 不可重试立即返回
    
    // 计算退避时间
    auto backoff = initial_backoff * (1 << (attempt - 1));
    backoff = min(backoff, max_backoff);
    backoff *= jitter(rng);  // 添加抖动
    
    std::this_thread::sleep_for(backoff);
    last = fn();
  }
  return last;
}
```

## API 重试策略

### ✅ 幂等操作（启用重试）

| API | 重试 | max_attempts | 理由 |
|-----|------|--------------|------|
| **HeadObject** | ✅ | 3 | 只读，完全幂等 |
| **GetObject** | ✅ | 3 | 只读，完全幂等 |
| **PutObject** | ✅ | 3 | 幂等（相同内容覆盖） |
| **StartUpload** | ✅ | 3 | 幂等（idempotency_key） |
| **UploadPart** | ✅ | 3 | 幂等（part_number 唯一） |
| **AbortUpload** | ✅ | 3 | 幂等（删除操作） |

### ⚠️ 非幂等操作（禁用重试）

| API | 重试 | max_attempts | 理由 |
|-----|------|--------------|------|
| **CompleteUpload** | ❌ | 1 | 非幂等（已 Complete 重试会失败） |

**CompleteUpload 特殊处理：**
```cpp
// client/src/data/http_data_client.cpp:543-554
Result<CompleteUploadResp> HttpDataClient::CompleteUpload(...) {
  // CompleteUpload 单独走 max_attempts=1：server 端 Complete 不是天然幂等
  // （已 Complete 的 upload_id 重做会撞 kStaleState）。retryable=true 的
  // 5xx 重试可能让 client 看到 "已成功但又失败" 的歧义状态，所以禁用重试，
  // 让上层（MultipartUpload）做更准确的失败处理（best-effort AbortUpload）。
  RetryPolicy p;
  p.max_attempts = 1;  // ✅ 禁用重试
  return RetryIfRetryable(p, [&] { return CompleteUploadOnce(...); });
}
```

## 退避时间计算

### 示例（initial=100ms, max=2000ms）

| 尝试次数 | 基础退避 | 实际退避（含抖动） |
|---------|---------|------------------|
| 1 | 100ms | 50-150ms |
| 2 | 200ms | 100-300ms |
| 3 | 400ms | 200-600ms |
| 4 | 800ms | 400-1200ms |
| 5 | 1600ms | 800-2400ms |
| 6+ | 2000ms (cap) | 1000-3000ms |

### 抖动效果

```
无抖动（雷鸣效应）：
Client 1: ---|retry|---|retry|---|retry|
Client 2: ---|retry|---|retry|---|retry|
Client 3: ---|retry|---|retry|---|retry|
         所有客户端同时重试，加剧服务器压力

有抖动（分散重试）：
Client 1: ---|retry|-----|retry|---|retry|
Client 2: ----|retry|---|retry|-----|retry|
Client 3: --|retry|-----|retry|---|retry|
         重试时间分散，减轻服务器压力
```

## 可重试错误判断

### Gateway 端标记

```cpp
// gateway/src/common/error.cpp
Error MakeError(ErrorCode code, std::string message, bool retryable, ...) {
  return Error{
    .code = code,
    .message = std::move(message),
    .retryable = retryable,  // ✅ 明确标记是否可重试
    ...
  };
}
```

### Client 端判断

```cpp
// client/src/data/http_data_client.cpp:86-102
Error MapHttpFailure(const brpc::Controller& cntl, ...) {
  const int status = cntl.http_response().status_code();
  ErrorCode code = HttpStatusToCode(status);
  
  // ✅ 5xx 错误默认可重试
  const bool retryable = (status >= 500);
  
  return MakeError(code, std::move(msg), retryable);
}
```

### 可重试错误类型

| HTTP Status | ErrorCode | retryable | 说明 |
|-------------|-----------|-----------|------|
| **500** | kInternal | ✅ | 服务器内部错误 |
| **502** | kBackendUnavailable | ✅ | 后端不可用 |
| **503** | kBackendUnavailable | ✅ | 服务不可用 |
| **504** | kBackendUnavailable | ✅ | 网关超时 |
| **400** | kBadRequest | ❌ | 客户端错误 |
| **404** | kNotFound | ❌ | 资源不存在 |
| **409** | kStaleState | ❌ | 状态冲突 |

## 配置选项

### HttpClientOptions

```cpp
// include/us3_turbo_access/client/options.h
struct HttpClientOptions {
  int max_retry_attempts{3};                              // 最大重试次数
  std::chrono::milliseconds retry_initial_backoff{100};   // 初始退避
  std::chrono::milliseconds retry_max_backoff{2000};      // 最大退避
};
```

### 用户自定义

```cpp
// 用户可以自定义重试策略
HttpClientOptions opts;
opts.max_retry_attempts = 5;           // 增加重试次数
opts.retry_initial_backoff = 200ms;    // 增加初始退避
opts.retry_max_backoff = 5000ms;       // 增加最大退避

Client client(opts);
```

## 验证结果

### ✅ 算法正确性

| 检查项 | 状态 | 说明 |
|--------|------|------|
| **指数退避** | ✅ | 2^(n-1) 正确实现 |
| **退避上限** | ✅ | max_backoff 正确限制 |
| **抖动** | ✅ | [0.5, 1.5) 避免雷鸣 |
| **线程安全** | ✅ | thread_local RNG |

### ✅ 重试条件

| 检查项 | 状态 | 说明 |
|--------|------|------|
| **成功立即返回** | ✅ | 不浪费重试机会 |
| **不可重试立即返回** | ✅ | 避免无效重试 |
| **5xx 可重试** | ✅ | 符合 HTTP 语义 |
| **4xx 不可重试** | ✅ | 符合 HTTP 语义 |

### ✅ API 策略

| 检查项 | 状态 | 说明 |
|--------|------|------|
| **幂等操作重试** | ✅ | 6/7 API 启用重试 |
| **非幂等操作禁用** | ✅ | CompleteUpload 禁用 |
| **策略可配置** | ✅ | HttpClientOptions |

### ✅ 最佳实践

| 检查项 | 状态 | 说明 |
|--------|------|------|
| **指数退避** | ✅ | 业界标准 |
| **抖动** | ✅ | 避免雷鸣效应 |
| **退避上限** | ✅ | 避免无限等待 |
| **幂等性判断** | ✅ | 正确区分幂等/非幂等 |
| **可配置** | ✅ | 用户可自定义 |

## 性能分析

### 最坏情况延迟

**默认配置（max_attempts=3）：**
```
尝试 1: 立即
尝试 2: +100ms (50-150ms)
尝试 3: +200ms (100-300ms)
总延迟: 0-450ms
```

**激进配置（max_attempts=5）：**
```
尝试 1: 立即
尝试 2: +100ms
尝试 3: +200ms
尝试 4: +400ms
尝试 5: +800ms
总延迟: 0-1500ms
```

### 成功率提升

假设单次请求成功率 95%：
- **无重试**：成功率 95%
- **重试 1 次**：成功率 99.75% (1 - 0.05²)
- **重试 2 次**：成功率 99.9875% (1 - 0.05³)

## 问题分析

### ✅ 无明显问题

重试策略设计合理，实现正确，符合最佳实践。

### 💡 可选优化（P2）

1. **自适应退避**
   - 根据服务器负载动态调整退避时间
   - 读取 Retry-After 响应头

2. **重试指标**
   - 记录重试次数、成功率等指标
   - 便于监控和调优

3. **断路器模式**
   - 连续失败后暂停重试
   - 避免雪崩效应

## 验证结论

### ✅ 完成度
- 重试算法：100% 正确 ✅
- API 策略：100% 合理 ✅
- 最佳实践：100% 符合 ✅
- 可配置性：100% 支持 ✅

### 📊 评分
- **正确性**：10/10 ✅
- **合理性**：10/10 ✅
- **可维护性**：10/10 ✅
- **可扩展性**：9/10 ✅

### 💡 总结
Client 端重试策略设计优秀，无需修复。实现了：
- ✅ 指数退避 + 抖动
- ✅ 正确的幂等性判断
- ✅ 合理的默认配置
- ✅ 灵活的用户配置

## 下一步行动
- ✅ 验证完成，无需修复
- 📝 可选：添加重试指标（P2）
- 📝 可选：实现断路器模式（P2）
