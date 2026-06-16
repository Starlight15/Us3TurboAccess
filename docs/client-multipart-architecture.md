# Client Multipart 架构说明

## 架构目标

让三条数据链路（HTTP / RDMA / GDS）在同一套公共 API 下各自独立演化：
- 共同遵守 `Client::StartUpload / UploadPart / Complete / Abort` 这层合约
- 中下层不受"统一参数形状"约束，各链路可以保留自己的协议语义

---

## 分层说明

```
Client
  └─ UploadCoordinator           按 DataPath 路由
       └─ IMultipartFlow         链路 session 工厂（HTTP / RDMA / GDS 各一个实例）
            └─ IMultipartSession 链路会话（每次 StartUpload 分配一个）
                 └─ TransferPath 链路数据面（PutObjectPart 等）
```

### Client
对外公共 API：`StartUpload → UploadPart / UploadParts → Complete / Abort`。
持有 `MultipartUpload`，后者通过 `Impl` 独占一个 `IMultipartSession`。

### MultipartUpload::Impl
- 持有 `unique_ptr<IMultipartSession>` —— session 负责内化 upload_id / max_part_size
- 只保留外层可变的会话字段：`checksum_policy`（由 `set_checksum_policy` 写入）
- `parts / bytes_committed / finished` 由 `mu` 保护，用于并发 UploadPart 的聚合

### UploadCoordinator
流 (flow) 注册表。按 `DataPath` 返回对应的 `IMultipartFlow`，不承载任何协议实现。
三条 flow 实例与 Client 同寿命。

### IMultipartFlow
Session 工厂接口，唯一方法 `CreateSession(ObjectDescriptor)`：
接收一次性会话描述符，完成链路打开逻辑（control plane RPC / 端点准备），
返回 `IMultipartSession`。

### IMultipartSession
链路会话接口：`UploadPart / Complete / Abort`。
upload_id、max_part_size、目标 ObjectId 全部内化，调用方只传 part 级变量。
三条链路各有独立实现，可保留链路特有字段。

### TransferPath
链路数据面（`HttpTransferPath / RdmaTransferPath / GdsTransferPath`）。
Multipart part 上传入口：`PutObjectPart(RequestOptions, buffer, upload_id, part_number)`。

---

## 三条链路对照

| | HTTP | RDMA | GDS |
|---|---|---|---|
| **打开 multipart** | `HttpDataClient::StartUpload` (HTTP) | `MetadataClient::RpcCreateMultipartUpload` (baidu_std) | 同 RDMA |
| **上传 part** | `HttpTransferPath::PutObjectPart` (HTTP PUT) | `RdmaTransferPath::PutObjectPart` (UCX ucp_put_nbx + CommitPart) | `GdsTransferPath::PutObjectPart` (GdsChunk，gateway 主动拉) |
| **完成** | `HttpDataClient::CompleteUpload` (HTTP) | `MetadataClient::RpcCompleteMultipartUpload` | 同 RDMA |
| **中止** | `HttpDataClient::AbortUpload` (HTTP) | `MetadataClient::RpcAbortMultipartUpload` | 同 RDMA |
| **Session 持有** | object + upload_id | object + upload_id | object + upload_id |
| **链路特有字段** | — | — | length 不设（跳过 Reserve） |

---

## 当前关键约束

**UploadParts 并发模型不可改成 bthread。**
`UploadPart` 是阻塞调用（HTTP 等 TCP ACK、RDMA 等 RMA WRITE + CommitPart、GDS 等 GdsChunk 返回）。
Worker 必须运行在独立 `std::thread`，不能迁移到 brpc bthread：bthread 阻塞会占住底层 pthread，
8 个并发 worker 足以耗尽 brpc pthread pool，拖垮控制面 RPC 调度。
（经过性能回归验证：bthread 版本 RDMA 吞吐从 ~18 GB/s 降至 ~640 MB/s。）

**三条链路中下层是隔离设计。**
`HttpMultipartFlow/Session` 不依赖 MetadataClient；`RdmaMultipartFlow/Session` 不依赖 HttpDataClient。
新增链路只需实现 `IMultipartFlow + IMultipartSession` 两个类，不影响现有链路。

---

## 当前调用链（简版）

```
Client::StartUpload(object, size, idempotency_key)
  → UploadCoordinator::SelectFlow(data_path)
  → IMultipartFlow::CreateSession(desc)           # 打开 multipart，返回 session
  → MultipartUpload{Impl{session}}                # session 独占持有

MultipartUpload::UploadPart(part_number, offset, buffer)
  → [lock] snapshot checksum_policy              # 唯一需锁的可变会话字段
  → IMultipartSession::UploadPart(part_number, offset, checksum_policy, buffer)
  → TransferPath::PutObjectPart(request, buffer, upload_id, part_number)
  → [unlock] RecordUploadedPart (part_number, etag)

MultipartUpload::Complete()
  → [lock] snapshot parts
  → IMultipartSession::Complete(parts)
  → control plane RPC

MultipartUpload::~MultipartUpload()
  → if !finished: IMultipartSession::Abort()     # best-effort
```
