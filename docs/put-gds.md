# PUT 链路：GDS

## 流程图

```
Client                                             Gateway (控制面 + 数据面合一)
  |                                                    |
  | PreflightGds(buffer_type=kCudaDevice)              |
  | [size > put_single_max_bytes → 拒绝]               |
  |                                                    |
  | RetryIfRetryable():                                |
  |   OpenSession (memory_registry.Register)           |
  |   MetadataClient::OpenTransferSession() ─────────→ ControlPlaneService::OpenSession()
  |     MakeSessionHandshake()                         |   SessionOpener::Open()
  |                                                    |   GdsExecutor::OnSessionOpened()
  |                           ←── session_id, ticket ──|     metadata_.Reserve() [expected_size≠0]
  |                                                    |
  |   GdsMemoryRegistry::AcquireToken()                |
  |     (cuMemObjGetRDMAToken, lazy register)          |
  |                                                    |
  |   ChunkDispatcher::Dispatch(token, offset, size) ──→ ControlPlaneService::GdsPut()
  |     GdsDataClient::GdsChunk(ChunkOp) [baidu_std]  |   HandleGdsPut()
  |                                                    |   ResolveSession() [session_id/ticket]
  |                                                    |   sessions_.BumpActive()
  |                                                    |   GdsExecutor::PutChunk()
  |                                                    |     server->allocateChannelId()
  |                                                    |     PinnedBufferPool::Acquire()
  |                                                    |     server->handlePutObject() [RDMA-READ GPU→pinned]
  |                                                    |     backend_.WriteRange()
  |                           ←── etag, version ───────|
  |   TransferOutcome 组装                              |
  |   [失败时] MetadataClient::AbortSession() ─────────→ ControlPlaneService::AbortSession()
  |                                                    |   sessions_.MarkFailed()
```

## 逐步说明

1. GdsTransferPath::PutObject (gds_transfer_path.cpp:170) — Preflight 校验 buffer 类型为 kCudaDevice，检查 put_single_max_bytes 上限。
2. RetryIfRetryable (gds_transfer_path.cpp:191) — 包裹整个 OpenSession+ExecutePut 重试循环，达 deadline 停止。
3. OpenSession/memory_registry.Register (gds_transfer_path.cpp:37) — 校验 buffer 类型合法性（descriptor 字段已从协议移除）。
4. MetadataClient::OpenTransferSession (gds_transfer_path.cpp:47) — 发 baidu_std RPC 到 ControlPlaneService::OpenSession，返回 session_id + ticket。
5. ControlPlaneService::HandleOpenSession (control_plane_service.cpp:112) — 转派 io_pool 异步执行，调用 SessionOpener::Open。
6. GdsExecutor::OnSessionOpened (gds_executor.cpp:132) — PUT 且 expected_size≠0 时调用 metadata_.Reserve 预扩容。
7. GdsMemoryRegistry::AcquireToken (cuobject_client.cpp:49) — 调 cuMemObjGetRDMAToken 取显存 RDMA token，未注册时 lazy register。
8. ChunkDispatcher::Dispatch (chunk_dispatcher.cpp:36) — 把 (token, offset, size) 封装成 GdsChunk RPC 发给 gateway。
9. GdsDataClient::GdsChunk (chunk_dispatcher.cpp:60) — 通过 baidu_std 协议发送 GdsChunkRequest。
10. ControlPlaneService::HandleGdsPut (control_plane_service.cpp:226) — 按 upload_id 是否为空分流到 PutChunk（单对象）或 PutPart（multipart）。
11. GdsExecutor::PutChunk (gds_executor.cpp:245) — 分配 pinned buffer，调 cuObjServer::handlePutObject 通过 RDMA-READ 从 GPU 拉数据到 pinned buffer，再调 backend_.WriteRange 落盘。
12. GdsExecutor::PutPart (gds_executor.cpp:323) — multipart 分支，落盘到 backend_.WritePart，checksum_policy=="md5" 时返回 chunk MD5。
13. multipart_.RegisterPart (control_plane_service.cpp:273) — 记录 part offset/size/etag 供 CompleteUpload 校验。
14. MetadataClient::AbortSession (gds_transfer_path.cpp:205) — PUT 失败时 best-effort 通知 gateway 提前回收 session，避免等 TTL sweep。

## 关键参数

| 参数名 | 类型 | 位置 | 说明 |
|---|---|---|---|
| put_single_max_bytes | uint64 | ClientOptions.gds | 单段 PUT 上限（与 cuObjServer 1 GiB chunk 限制对齐），超出客户端拒绝 |
| max_chunk_bytes | uint64 | GdsOptions | gateway 侧单次 RDMA chunk 上限（1 GiB），超出返回 kBadRequest |
| buffer_size_classes | vector | GdsOptions | PinnedBufferPool size class 列表，决定 pinned buffer 池分级 |
| buffer_max_per_class | size_t | GdsOptions | 每 size class 最大池容量，影响并发 PUT 峰值内存 |
| rdma_port | uint16 | GdsOptions | cuObjServer 监听的 RDMA 端口 |
| checksum_policy | string | GdsChunkRequest | "md5" 时返回 chunk MD5 作为 part etag（多 chunk 切分时取最后一块）|

## 错误处理

- buffer 类型不为 kCudaDevice：PreflightGds 返回 kUnsupportedPath，不发 RPC。
- PUT body 超过 put_single_max_bytes：客户端 PutObject 直接返回 kPayloadTooLarge。
- gateway GDS 不可用（cuObjServer 未启动）：HandleGdsPut 返回 "gds-cuobject service is not available"，客户端按 retryable 重试。
- session 不存在：ResolveSession 返回 nullptr，HandleGdsPut 返回错误并计入 gds_put_fail_total。
- RDMA token 为空：HandleGdsPut 拒绝请求，返回 "missing rdma token"。
- cuObjServer pinned buffer 分配失败：PutChunk 返回 kRdmaUnavailable，client 重试。
- handlePutObject RDMA 失败（ibv_wc_status ≠ SUCCESS）：PutChunk 返回 kRpcError，sessions_.MarkFailed 标记失败。
- backend_.WriteRange 失败：PutChunk 返回 Failure，gateway 侧 sessions_.MarkFailed，客户端重试时重新 OpenSession。
- 重试超时（deadline）：RetryIfRetryable 返回 kTimeout，不再重试。
