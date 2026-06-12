# PUT 链路：HTTP

## 流程图

```
Client                                          Gateway
  |                                                |
  | Preflight(buffer_type=kHostRegular)            |
  |-- [size > put_single_max_bytes → 拒绝]         |
  |                                                |
  | ComputeClientCrc32c()                          |
  | HttpDataClient::PutObject()  ──── PUT /v1/objects/{bucket}/{key} ──→ |
  |   └─ PutObjectOnce()         [x-amz-checksum-crc32c header]          |
  |       └─ RetryIfRetryable()                    |                      |
  |                                                | HttpFrontend::default_method()
  |                                                | ParseObjectPath()
  |                                                | ParseContentLengthHeader() → [>max_put_bytes_ → 413]
  |                                                | ParseCrc32cHeader()
  |                                                | HttpFrontend::HandlePut()
  |                                                |   HttpExecutor::Put(IOBuf)
  |                                                |     backend_.Reserve()
  |                                                |     backend_.WriteRange() [per IOBuf block]
  |                                                |     Crc32cUpdate() [single-pass]
  |                                                |     VerifyCrc32c() [client vs server]
  |←── 200 OK  {etag, version, bytes_written} ────|
  |    x-amz-checksum-crc32c: EncodeCrc32cBase64() |
```

## 逐步说明

1. HttpTransferPath::PutObject (http_transfer_path.cpp:178) — 检查 buffer 类型为 kHostRegular，大小不超过 put_single_max_bytes。
2. ComputeClientCrc32c (http_transfer_path.cpp:195) — 当 send_crc32c=true 时计算 buffer 的 CRC32C。
3. HttpDataClient::PutObject (http_data_client.h:98) — 公开接口，套 RetryIfRetryable 做指数退避重试。
4. PutObjectOnce (http_data_client.h:134) — 实际发起单次 HTTP PUT RPC，不做 retry。
5. HttpFrontend::default_method (http_frontend.cpp:270) — brpc 入口，路由 PUT 到 HandlePut。
6. ParseContentLengthHeader (http_frontend.cpp:220) — 解析 Content-Length，超 max_put_bytes_ 返回 413。
7. ParseCrc32cHeader (http_frontend.cpp:163) — 从 x-amz-checksum-crc32c header 解码 base64 CRC32C。
8. HttpFrontend::HandlePut (http_frontend.cpp:491) — 验证 body 大小，调用 HttpExecutor::Put(IOBuf)。
9. HttpExecutor::Put/IOBuf (http_executor.cpp:173) — 调用 backend_.Reserve 预分配，逐 IOBuf block 执行 WriteRange 并同步计算 CRC32C。
10. VerifyCrc32c (http_executor.cpp:31) — 对比 client 提交的 CRC32C 与 server 实算值，不一致立即返回错误。
11. EncodeCrc32cBase64 (http_frontend.cpp:197) — 将 server 计算的 CRC32C 以 base64 写入响应头 x-amz-checksum-crc32c。

## 关键参数

| 参数名 | 类型 | 位置 | 说明 |
|---|---|---|---|
| put_single_max_bytes | uint64 | ClientOptions.http | 单段 PUT 最大字节数（默认 5 GiB），超出时客户端拒绝 |
| max_put_bytes_ | size_t | HttpFrontend | gateway 侧 PUT body 上限，超出返回 413 |
| send_crc32c | bool | ClientOptions.http | 是否在请求头附带 CRC32C，启用时 server 端校验 |
| parallel_get_threshold | uint64 | ClientOptions.http | GET 并发分片阈值（PUT 链路不涉及，仅 GET） |
| kChunkBytes | size_t | http_executor.cpp:19 | GET 流式读写每块大小（1 MiB），PUT 路径按 IOBuf block 大小写入 |

## 错误处理

- 客户端 buffer 类型不为 kHostRegular：Preflight 返回 kUnsupportedPath，不发 RPC。
- PUT body 超过 put_single_max_bytes：客户端 PutObject 直接返回 kPayloadTooLarge，提示改用分片上传。
- gateway 接收 body 超过 max_put_bytes_：HandlePut 返回 HTTP 413，WriteError 写 JSON 错误体。
- CRC32C 校验失败：HttpExecutor::Put 返回 kInvalidArgument，gateway 返回 4xx，客户端 RetryIfRetryable 判断不可重试。
- backend_.Reserve / WriteRange 失败：HttpExecutor::Put 返回 Failure，gateway 返回 5xx，客户端按 retryable 标记决定是否重试。
- 网络超时或连接失败：HttpChannel RPC 失败，PutObjectOnce 返回 Failure，RetryIfRetryable 指数退避重试。
