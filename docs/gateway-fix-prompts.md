# Gateway P0 修复提示词

---

## P0-1: MultipartStore::Touch 竞态修复

### 问题
文件 `gateway/src/core/multipart/multipart_store.cpp` 行 50-53：

```cpp
void MultipartStore::Touch(MultipartUpload& upload,
                           std::chrono::steady_clock::time_point now) {
  upload.last_activity_at = now;  // 无锁！
}
```

`SweepExpired`（行 73-76）读取 `last_activity_at` 时持有 `upload.mu`，但 `Touch` 写入时无锁，存在竞态。

### 修改要求

1. 查看 `gateway/src/core/multipart/multipart_store.h`，确认 `MultipartUpload` 结构体的 `last_activity_at` 字段类型和 `mu` 成员

2. 修改 `multipart_store.cpp` 的 `Touch` 函数，加锁保护写入：
   ```cpp
   void MultipartStore::Touch(MultipartUpload& upload,
                              std::chrono::steady_clock::time_point now) {
     std::scoped_lock lock(upload.mu);
     upload.last_activity_at = now;
   }
   ```

3. **不要修改 SweepExpired**，它已正确持有 `upload.mu`。

### 验证要点
- 编译通过
- Touch 和 SweepExpired 不再有竞态

---

## P0-2: UcxExecutor::PrepareTransfer 幂等性校验

### 问题
文件 `gateway/src/data_path/ucx/ucx_executor.cpp` 行 319-328：

```cpp
// 幂等：已分配过直接返回
if (entry->slot) {
  UcxDiscoverInfo info;
  info.host           = public_host_;
  // ...
  info.gw_raddr       = entry->slot->gw_raddr;
  info.gw_packed_rkey = entry->slot->packed_rkey;
  return Result<UcxDiscoverInfo>::Success(std::move(info));
}
```

当 client 重试时，若 `transfer_bytes` 与第一次不同（例如 client bug 或协议变更），已分配的 slot 可能小于本次请求的 `transfer_bytes`，RDMA 写入越界。

### 修改要求

在 `if (entry->slot)` 分支内，添加 `transfer_bytes` 一致性校验：

```cpp
// 幂等：已分配过直接返回
if (entry->slot) {
  // 校验重试时 transfer_bytes 与首次一致，防止 RDMA 写越界
  if (entry->transfer_bytes != static_cast<std::size_t>(transfer_bytes)) {
    return Result<UcxDiscoverInfo>::Failure(common::MakeError(
        ErrorCode::kBadRequest,
        "PrepareTransfer: transfer_bytes mismatch on retry: expected " +
            std::to_string(entry->transfer_bytes) + " got " +
            std::to_string(transfer_bytes)));
  }
  UcxDiscoverInfo info;
  info.host           = public_host_;
  info.ucx_port       = static_cast<std::uint32_t>(opts_.listen_port);
  info.max_msg_bytes  = opts_.max_msg_bytes;
  info.gw_raddr       = entry->slot->gw_raddr;
  info.gw_packed_rkey = entry->slot->packed_rkey;
  return Result<UcxDiscoverInfo>::Success(std::move(info));
}
```

### 注意事项
- `entry->transfer_bytes` 在首次分配时（行 378-379）设为 `static_cast<std::size_t>(transfer_bytes)`，类型一致
- 错误码使用 `ErrorCode::kBadRequest`，不可重试（client 逻辑错误）
- 错误消息包含期望值和实际值，便于诊断

### 验证要点
- 编译通过
- 相同 `transfer_bytes` 的重试正常返回
- 不同 `transfer_bytes` 的重试返回 kBadRequest 错误
