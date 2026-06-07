# PUT 链路：RDMA-UCX

## 流程图

```
Client                                              Gateway (控制面 + 数据面分离)
  |                                                     |
  | RdmaTransferPath::PutObject()                       |
  | [size > rdma.max_msg_bytes → 拒绝]                  |
  |                                                     |
  | RetryIfRetryable():                                 |
  |   PrepareAndWrite():                                |
  |     1) MetadataClient::OpenTransferSession() ──────→ ControlPlaneService::OpenSession()
  |        MakeSessionHandshake(kNativeRdma)            |   UcxExecutor::OnSessionOpened()
  |                             ←── session_id ─────────|     session_registry_->Create()
  |                                                     |
  |     2) RdmaDataPlaneClient::DiscoverEndpoint() ────→ RdmaDataPlaneService::DiscoverRdmaEndpoint()
  |        [buffer.size > max_msg_bytes → AbortSession] |   UcxExecutor::DiscoverEndpoint()
  |                                                     |     buffer_pool_->Acquire() / aligned_alloc()
  |                             ←── {ucx_port, tag} ───|     ucp_tag_recv_nbx() [TAG recv 就绪]
  |                                                     |     ProgressLoop [后台 spin]
  |     3) UcxEndpointPool::Acquire(host, port)         |
  |        [池命中: ~0ms; 无空闲: ucp_ep_create ~8ms]   |
  |                                                     |
  |     4) UcxEndpointOps::SendAsync() ─── UCX TAG ───→ [ProgressLoop: ucp_worker_progress]
  |        ucp_tag_send_nbx(ep, buf, size, tag)         |   OnSendComplete → data_ready=true
  |        [等待 future，超时 → Discard ep]              |
  |                                                     |
  |     5) UcxEndpointOps::Flush()                      |
  |        UcxEndpointPool::Return(ep)                  |
  |                                                     |
  |   ComputeClientCrc32c() → Base64Crc32cBigEndian()   |
  |   RdmaDataPlaneClient::CommitObject() ─────────────→ RdmaDataPlaneService::CommitObject()
  |      [失败 → AbortSession]                          |   UcxExecutor::CommitObject()
  |                                                     |     等待 data_ready（sched_yield+deadline）
  |                                                     |     VerifyCrc32c(view, client_crc32c_b64)
  |                                                     |     backend_.WriteRange()
  |                             ←── {etag, version} ───|
  | TransferOutcome 组装                                 |
```

## 逐步说明

1. RdmaTransferPath::PutObject (rdma_transfer_path.cpp:151) — 检查 available()（UCX 已初始化），body 超 rdma.max_msg_bytes 直接拒绝。
2. RetryIfRetryable (rdma_transfer_path.cpp:163) — 包裹 PrepareAndWrite + CommitObject，到 deadline 停止重试。
3. PrepareAndWrite (rdma_transfer_path.cpp:56) — 串联 OpenSession、DiscoverEndpoint、TAG send、Flush 五步，任一超时先 AbortSession 再返回错误。
4. MetadataClient::OpenTransferSession (rdma_transfer_path.cpp:72) — 向控制面发 baidu_std RPC，返回 session_id/request_id/gateway_id。
5. UcxExecutor::OnSessionOpened (ucx_executor.cpp:256) — 在 session_registry_ 中创建条目，记录 bucket/key/expected_bytes。
6. RdmaDataPlaneClient::DiscoverEndpoint (rdma_transfer_path.cpp:92) — 向数据面 RPC 查询 UCX 监听端口和接收 TAG。
7. UcxExecutor::DiscoverEndpoint (ucx_executor.cpp:278) — 分配接收 buffer（优先 buffer_pool_，否则 aligned_alloc），调 ucp_tag_recv_nbx 投递 TAG recv；幂等，已分配直接返回。
8. UcxEndpointPool::Acquire (ucx_endpoint_pool.h:39) — 从 idle_ 中取可用 ep；无空闲时新建连接，失败返回错误。
9. UcxEndpointOps::SendAsync (ucx_endpoint.cpp:25) — 调 ucp_tag_send_nbx 异步发送，通过 promise/future 等待 OnSendComplete 回调。
10. UcxEndpointOps::Flush (ucx_endpoint.cpp:53) — 调 ucp_ep_flush_nbx 确保数据飞出 NIC，最多轮询 1000 次 ucp_worker_progress。
11. UcxEndpointPool::Return (ucx_endpoint_pool.h:42) — 成功后归还 ep；超出 max_idle 时丢弃。
12. ComputeClientCrc32c / Base64Crc32cBigEndian (rdma_transfer_path.cpp:174) — 计算 buffer CRC32C 并编码为 base64，随 CommitObject 发给 gateway。
13. RdmaDataPlaneClient::CommitObject (rdma_transfer_path.cpp:178) — 发 CommitObject RPC，通知 gateway 数据已发完并提供 CRC32C。
14. UcxExecutor::CommitObject (ucx_executor.cpp:378) — 等待 data_ready（sched_yield 轮询，超时返回 kTimeout），校验 CRC32C，调 backend_.WriteRange 落盘。
15. UcxExecutor::CommitPart (ucx_executor.cpp:431) — multipart 分支：等 data_ready，校验 CRC32C，调 backend_.WritePart 并 RegisterPart。

## 关键参数

| 参数名 | 类型 | 位置 | 说明 |
|---|---|---|---|
| rdma.max_msg_bytes | uint64 | ClientOptions.rdma | 单次 UCX PUT 最大字节数，超出客户端拒绝，gateway 侧 DiscoverEndpoint 也检查 |
| pool_max_idle_per_endpoint | size_t | ClientOptions.rdma | UcxEndpointPool 每个 endpoint 最大空闲连接数，影响连接复用效率 |
| send_crc32c | bool | ClientOptions.rdma | 是否随 CommitObject 发送 CRC32C，gateway 端 VerifyCrc32c 校验 |
| opts_.listen_port | uint16 | UcxOptions | gateway UCX 监听端口，DiscoverEndpoint 返回给 client |
| opts_.max_msg_bytes | uint64 | UcxOptions | gateway 侧单 PUT 上限，DiscoverEndpoint 返回给 client 做前置拒绝 |
| opts_.commit_data_timeout | duration | UcxOptions | CommitObject/CommitPart 等待 data_ready 的超时时间 |
| opts_.buffer_pool_max_idle | size_t | UcxOptions | gateway 侧 UcxBufferPool 最大空闲 buffer 数，影响内存占用与 alloc 成本 |

## 错误处理

- UCX 未初始化（ucx_pool_ 为空）：PutObject 返回 kUnsupportedPath，不发任何 RPC。
- body 超过 rdma.max_msg_bytes：PutObject 返回 kPayloadTooLarge（不可重试）。
- OpenSession 失败：PrepareAndWrite 返回错误，RetryIfRetryable 按 retryable 重试。
- DiscoverEndpoint 失败或 ucx_port=0：PrepareAndWrite 调 AbortSession 清理 session，返回错误。
- buffer 超过 gateway max_msg_bytes：PrepareAndWrite 调 AbortSession，返回 kInvalidArgument。
- TAG send 超时（future.wait_until 超时）：Discard ep（不归还），AbortSession，返回 kTimeout（可重试）。
- ucp_tag_send_nbx 失败（OnSendComplete status≠UCS_OK）：Discard ep，AbortSession，返回传输错误。
- CommitObject 等待 data_ready 超时：UcxExecutor 返回 kTimeout，ucx_put_timeout_total 计数，客户端 AbortSession。
- CommitObject CRC32C 校验失败：VerifyCrc32c 返回 kInvalidArgument，ReleaseEntry 归还 buffer，客户端重试时重新 OpenSession。
- backend_.WriteRange 失败：CommitObject 返回 Failure，客户端 AbortSession 并按 retryable 重试。
- 重试达 deadline：RetryIfRetryable 返回 kTimeout，终止重试。
