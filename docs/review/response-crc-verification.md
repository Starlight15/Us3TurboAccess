# 响应 CRC 验证报告

## 验证目标
验证 Gateway 和 Client 之间的 CRC32C 响应流程是否完整正确。

## Gateway 端实现

### 1. CRC 计算（HttpExecutor）

**Put 操作：**
```cpp
// gateway/src/data_path/http/http_executor.cpp:167-219
Result<TransferReport> HttpExecutor::Put(..., const butil::IOBuf& body, ...) {
  // 单遍优化：边算 CRC 边写入
  std::uint32_t crc_state = common::Crc32cInit();
  for (block : body.backing_blocks()) {
    crc_state = common::Crc32cUpdate(crc_state, block.data(), block.size());
    backend_.WriteRange(...);
  }
  const std::uint32_t actual_crc = common::Crc32cFinalize(crc_state);
  
  // 返回 CRC
  report.crc32c = actual_crc;
  report.has_crc32c = true;
}
```

**PutPart 操作：**
```cpp
// gateway/src/data_path/http/http_executor.cpp:220-279
Result<TransferReport> HttpExecutor::PutPart(..., const butil::IOBuf& body, ...) {
  // 同样的单遍优化
  std::uint32_t crc_state = common::Crc32cInit();
  for (block : body.backing_blocks()) {
    crc_state = common::Crc32cUpdate(crc_state, block.data(), block.size());
    backend_.WritePart(...);
  }
  const std::uint32_t actual_crc = common::Crc32cFinalize(crc_state);
  
  report.crc32c = actual_crc;
  report.has_crc32c = true;
}
```

### 2. CRC 返回（HttpFrontend）

**Put 响应：**
```cpp
// gateway/src/api/http_frontend.cpp:513-516
if (report.value().has_crc32c) {
  cntl->http_response().SetHeader(
      "x-amz-checksum-crc32c", EncodeCrc32cBase64(report.value().crc32c));
}
```

**PutPart 响应：**
```cpp
// gateway/src/api/http_frontend.cpp:589-592
if (report.value().has_crc32c) {
  cntl->http_response().SetHeader(
      "x-amz-checksum-crc32c", EncodeCrc32cBase64(report.value().crc32c));
}
```

### 3. CRC 编码

```cpp
// gateway/src/api/http_frontend.cpp:162-170
std::string EncodeCrc32cBase64(std::uint32_t crc) {
  // S3 兼容：big-endian uint32 的 base64
  const std::uint32_t be = htonl(crc);
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&be);
  return butil::base64_encode(bytes, 4);
}
```

## Client 端实现

### 1. CRC 提取

```cpp
// client/src/data/http_data_client.cpp:107-111
std::optional<std::uint32_t> ExtractServerCrc32c(const brpc::Controller& cntl) {
  const auto* h = cntl.http_response().GetHeader("x-amz-checksum-crc32c");
  if (h == nullptr || h->empty()) return std::nullopt;
  return DecodeBase64Crc32cBigEndian(*h);
}
```

### 2. CRC 解码

```cpp
// client/src/transports/http/http_utils.cpp
std::optional<std::uint32_t> DecodeBase64Crc32cBigEndian(std::string_view encoded) {
  std::string decoded;
  if (!butil::base64_decode(encoded, &decoded) || decoded.size() != 4) {
    return std::nullopt;
  }
  std::uint32_t be;
  std::memcpy(&be, decoded.data(), 4);
  return ntohl(be);  // big-endian → host
}
```

### 3. CRC 使用

**PutObject：**
```cpp
// client/src/data/http_data_client.cpp:278-282
PutReport out;
out.bytes         = buffer.size;
out.meta          = ExtractMeta(cntl);
out.server_crc32c = ExtractServerCrc32c(cntl);  // ✅ 提取 server CRC
return Result<PutReport>::Success(std::move(out));
```

**PutPart：**
```cpp
// client/src/data/http_data_client.cpp:406-410
PutPartReport out;
out.bytes         = buffer.size;
out.part_etag     = ExtractMeta(cntl).etag;
out.server_crc32c = ExtractServerCrc32c(cntl);  // ✅ 提取 server CRC
return Result<PutPartReport>::Success(std::move(out));
```

## 验证结果

### ✅ 完整性检查

| 步骤 | Gateway | Client | 状态 |
|------|---------|--------|------|
| **1. CRC 计算** | ✅ 单遍优化 | N/A | ✅ |
| **2. CRC 编码** | ✅ Base64(BE) | N/A | ✅ |
| **3. CRC 返回** | ✅ x-amz-checksum-crc32c | N/A | ✅ |
| **4. CRC 提取** | N/A | ✅ ExtractServerCrc32c | ✅ |
| **5. CRC 解码** | N/A | ✅ DecodeBase64Crc32cBigEndian | ✅ |
| **6. CRC 使用** | N/A | ✅ PutReport.server_crc32c | ✅ |

### ✅ 对称性检查

| 操作 | Gateway 编码 | Client 解码 | 状态 |
|------|-------------|-------------|------|
| **编码格式** | Base64(big-endian uint32) | Base64 → big-endian → host | ✅ |
| **字节序** | htonl (host → BE) | ntohl (BE → host) | ✅ |
| **头名称** | x-amz-checksum-crc32c | x-amz-checksum-crc32c | ✅ |

### ✅ 覆盖范围

| API | Gateway 返回 CRC | Client 提取 CRC | 状态 |
|-----|-----------------|----------------|------|
| **PutObject** | ✅ | ✅ | ✅ |
| **PutPart** | ✅ | ✅ | ✅ |
| **GetObject** | ❌ (未实现) | ❌ (未实现) | ⚠️ 不在 PUT 范围内 |

## 端到端流程验证

### PutObject 流程

```
Client                          Gateway
  |                                |
  |-- PUT /v1/objects/... -------->|
  |   (body + x-amz-checksum-crc32c)|
  |                                |
  |                          [计算 CRC]
  |                          crc = Crc32cInit()
  |                          for block:
  |                            crc = Update(crc, block)
  |                          crc = Finalize(crc)
  |                                |
  |<-- 200 OK -------------------|
  |    x-amz-checksum-crc32c: <base64>
  |                                |
[解码 CRC]                         |
server_crc = Decode(header)        |
  |                                |
[上层验证]                          |
if (client_crc != server_crc)      |
  → 数据损坏                        |
```

### PutPart 流程

```
Client                          Gateway
  |                                |
  |-- PUT /v1/uploads/{id} ------->|
  |   (body + x-amz-checksum-crc32c)|
  |                                |
  |                          [计算 CRC]
  |                          (同 PutObject)
  |                                |
  |<-- 200 OK -------------------|
  |    x-amz-checksum-crc32c: <base64>
  |                                |
[解码 CRC]                         |
server_crc = Decode(header)        |
```

## 问题分析

### ⚠️ 潜在问题

1. **Client 端不验证 CRC**
   - 问题：Client 提取了 server_crc32c，但没有自动验证
   - 影响：需要上层应用手动验证
   - 建议：在 Client 内部添加可选的自动验证

2. **GetObject 未实现 CRC**
   - 问题：下载路径没有 CRC 校验
   - 影响：无法检测下载数据损坏
   - 建议：P2 优先级，添加 GetObject CRC 支持

### ✅ 优点

1. **S3 兼容**：使用标准的 x-amz-checksum-crc32c 头
2. **高效计算**：单遍优化，边写边算
3. **正确编码**：big-endian + base64，符合 S3 规范
4. **完整覆盖**：PUT 和 PutPart 都支持

## 验证结论

### ✅ 核心功能完整
- Gateway 正确计算和返回 CRC ✅
- Client 正确提取和解码 CRC ✅
- 编码格式对称且符合 S3 规范 ✅
- PUT 和 PutPart 都已覆盖 ✅

### 📊 完成度
- 响应 CRC 流程：100% ✅
- 端到端验证：可用（需上层应用验证）
- S3 兼容性：100% ✅

### 💡 改进建议（可选）

1. **Client 端自动验证**（P2）
```cpp
// 在 PutObject 内部添加可选验证
if (config.verify_crc && out.server_crc32c.has_value()) {
  if (client_crc != *out.server_crc32c) {
    return Error{kDataCorruption, "CRC mismatch"};
  }
}
```

2. **GetObject CRC 支持**（P2）
```cpp
// 下载时也计算和返回 CRC
Result<GetReport> GetObject(...) {
  uint32_t crc = Crc32cInit();
  // 边读边算 CRC
  out.server_crc32c = crc;
}
```

## 下一步行动
- ✅ 验证完成，无需修复
- 📝 文档化 CRC 验证流程（可选）
- 🔄 P2 阶段考虑添加自动验证（可选）
