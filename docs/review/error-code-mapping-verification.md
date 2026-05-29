# 错误码映射验证报告

## 验证目标
验证 Gateway 和 Client 之间的错误码映射是否对称且完整。

## Gateway 端映射（ToHttpStatus）

| ErrorCode | HTTP Status | 说明 |
|-----------|-------------|------|
| kSuccess | 200 | OK |
| kInvalidArgument | 400 | Bad Request |
| kBadRequest | 400 | Bad Request |
| kUnsupported | 501 | Not Implemented |
| kNotFound | 404 | Not Found |
| kSessionNotFound | 404 | Not Found |
| kRangeNotSatisfiable | 416 | Range Not Satisfiable |
| kPayloadTooLarge | 413 | Payload Too Large |
| kCapacityExceeded | 507 | Insufficient Storage |
| kStaleState | 409 | Conflict |
| kTicketInvalid | 401 | Unauthorized |
| kBackendUnavailable | 502 | Bad Gateway |
| kRdmaUnavailable | 503 | Service Unavailable |
| kMethodNotAllowed | 405 | Method Not Allowed |
| kRpcError | 500 | Internal Server Error |
| kSerializationError | 500 | Internal Server Error |
| kControlPlaneError | 500 | Internal Server Error |
| kRegistrationFailed | 500 | Internal Server Error |
| kTransportError | 500 | Internal Server Error |
| kInternal | 500 | Internal Server Error |

## Client 端映射（HttpStatusToCode）

| HTTP Status | ErrorCode | 说明 |
|-------------|-----------|------|
| 404 | kNotFound | Not Found |
| 400 | kBadRequest | Bad Request |
| 416 | kRangeNotSatisfiable | Range Not Satisfiable |
| 413 | kPayloadTooLarge | Payload Too Large |
| 409 | kStaleState | Conflict |
| 507 | kCapacityExceeded | Insufficient Storage |
| 401 | kControlPlaneError | Unauthorized |
| 403 | kControlPlaneError | Forbidden |
| 500 | kInternal | Internal Server Error |
| 503 | kBackendUnavailable | Service Unavailable |
| 504 | kBackendUnavailable | Gateway Timeout |
| 其他 5xx | kInternal | 默认内部错误 |
| 其他 4xx | kBadRequest | 默认客户端错误 |

## 对称性验证

### ✅ 完全对称的映射

| ErrorCode | Gateway → HTTP | Client ← HTTP | 状态 |
|-----------|----------------|---------------|------|
| kNotFound | 404 | 404 → kNotFound | ✅ |
| kBadRequest | 400 | 400 → kBadRequest | ✅ |
| kRangeNotSatisfiable | 416 | 416 → kRangeNotSatisfiable | ✅ |
| kPayloadTooLarge | 413 | 413 → kPayloadTooLarge | ✅ |
| kStaleState | 409 | 409 → kStaleState | ✅ |
| kCapacityExceeded | 507 | 507 → kCapacityExceeded | ✅ |
| kInternal | 500 | 500 → kInternal | ✅ |

### ⚠️ 部分对称的映射

| ErrorCode | Gateway → HTTP | Client ← HTTP | 问题 |
|-----------|----------------|---------------|------|
| kInvalidArgument | 400 | 400 → kBadRequest | ⚠️ 映射到不同的 ErrorCode |
| kTicketInvalid | 401 | 401 → kControlPlaneError | ⚠️ 映射到不同的 ErrorCode |
| kBackendUnavailable | 502 | 503/504 → kBackendUnavailable | ⚠️ HTTP 码不匹配 |
| kRdmaUnavailable | 503 | 503 → kBackendUnavailable | ⚠️ 映射到不同的 ErrorCode |

### ❌ 单向映射（仅 Gateway 发送）

| ErrorCode | Gateway → HTTP | Client 处理 | 说明 |
|-----------|----------------|-------------|------|
| kUnsupported | 501 | 501 → kInternal | 回退到 kInternal |
| kSessionNotFound | 404 | 404 → kNotFound | 合并到 kNotFound |
| kMethodNotAllowed | 405 | 405 → kBadRequest | 回退到 kBadRequest |
| kRpcError | 500 | 500 → kInternal | 合并到 kInternal |
| kSerializationError | 500 | 500 → kInternal | 合并到 kInternal |
| kControlPlaneError | 500 | 500 → kInternal | 合并到 kInternal |
| kRegistrationFailed | 500 | 500 → kInternal | 合并到 kInternal |
| kTransportError | 500 | 500 → kInternal | 合并到 kInternal |

## 问题分析

### 1. kInvalidArgument vs kBadRequest
- **问题**：Gateway 发送 kInvalidArgument (400)，Client 收到后映射为 kBadRequest
- **影响**：语义略有差异，但都表示客户端错误，影响较小
- **建议**：保持现状，或统一使用 kBadRequest

### 2. kTicketInvalid → 401 → kControlPlaneError
- **问题**：Gateway 发送 kTicketInvalid (401)，Client 收到后映射为 kControlPlaneError
- **影响**：语义丢失，无法区分鉴权失败和控制面错误
- **建议**：Client 端添加 401 → kTicketInvalid 的映射

### 3. kBackendUnavailable vs kRdmaUnavailable
- **问题**：
  - Gateway: kBackendUnavailable → 502, kRdmaUnavailable → 503
  - Client: 502 → kInternal, 503 → kBackendUnavailable
- **影响**：502 被映射为 kInternal，语义丢失
- **建议**：Client 端添加 502 → kBackendUnavailable 的映射

### 4. 501/405 等特殊状态码
- **问题**：Client 端没有专门处理，回退到默认映射
- **影响**：语义丢失，但这些是边缘情况
- **建议**：可选优化，添加专门映射

## 修复建议

### 高优先级修复

```cpp
// client/src/data/http_data_client.cpp
ErrorCode HttpStatusToCode(int status) {
  switch (status) {
    case brpc::HTTP_STATUS_NOT_FOUND:                return ErrorCode::kNotFound;
    case brpc::HTTP_STATUS_BAD_REQUEST:              return ErrorCode::kBadRequest;
    case brpc::HTTP_STATUS_REQUEST_RANGE_NOT_SATISFIABLE:
                                                       return ErrorCode::kRangeNotSatisfiable;
    case brpc::HTTP_STATUS_REQUEST_ENTITY_TOO_LARGE:  return ErrorCode::kPayloadTooLarge;
    case brpc::HTTP_STATUS_CONFLICT:                  return ErrorCode::kStaleState;
    case 507:                                          return ErrorCode::kCapacityExceeded;
    
    // 修复 1: 添加 401 → kTicketInvalid
    case brpc::HTTP_STATUS_UNAUTHORIZED:              return ErrorCode::kTicketInvalid;
    
    // 修复 2: 添加 502 → kBackendUnavailable
    case brpc::HTTP_STATUS_BAD_GATEWAY:               return ErrorCode::kBackendUnavailable;
    
    case brpc::HTTP_STATUS_FORBIDDEN:                return ErrorCode::kControlPlaneError;
    case brpc::HTTP_STATUS_INTERNAL_SERVER_ERROR:    return ErrorCode::kInternal;
    case brpc::HTTP_STATUS_SERVICE_UNAVAILABLE:
    case brpc::HTTP_STATUS_GATEWAY_TIMEOUT:          return ErrorCode::kBackendUnavailable;
    default:
      return (status >= 500) ? ErrorCode::kInternal : ErrorCode::kBadRequest;
  }
}
```

### 低优先级优化

```cpp
// 可选：添加更多特殊状态码映射
case brpc::HTTP_STATUS_NOT_IMPLEMENTED:          return ErrorCode::kUnsupported;
case brpc::HTTP_STATUS_METHOD_NOT_ALLOWED:       return ErrorCode::kMethodNotAllowed;
```

## 验证结论

### ✅ 已完成
- 核心错误码（404/400/416/413/409/507/500）映射对称 ✅
- Action plan 中要求的 507/409/416/401 都已实现 ✅

### ⚠️ 需要修复
- 401 → kTicketInvalid（高优先级）
- 502 → kBackendUnavailable（高优先级）

### 📊 完成度
- 核心映射：7/7 (100%) ✅
- 完全对称：7/20 (35%)
- 可用性：良好（核心场景已覆盖）

## 下一步行动
1. 修复 401 和 502 的映射（15 分钟）
2. 添加单元测试验证对称性（可选）
3. 更新文档说明映射规则（可选）
