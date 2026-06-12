# 分片上传性能对比测试报告

> 测试日期：2026-06-07  
> 测试机器：192.168.1.198  
> 硬件：Mellanox ConnectX-6 (MT4125) × 4，单卡 200 Gbps (21915 MB/s 理论峰值)  
> 软件：UCX + brpc，Gateway backend=null，Client CRC32C 全关

---

## 一、测试目标

在**控制变量**前提下，量化 HTTP / RDMA (UCX RMA WRITE) / GDS (cuFile) 三条分片上传链路在以下维度上的差距：

- **吞吐量**（MiB/s）
- **端到端延迟**（p50 / p95 / p99）
- **Client 侧 CPU**（user + sys + %）
- **Gateway 侧内存增长**（RSS）

同时记录关闭 CRC32C 校验后 RDMA 链路的性能提升幅度，为生产参数选型提供数据支撑。

---

## 二、测试配置

### 2.1 控制变量

| 参数 | 值 |
|------|----|
| 对象大小 | 512 MB / 4 GB |
| Part 大小 | 64 MB (512MB 对象) / 128 MB (4GB 对象) |
| 并发线程 | 8 threads |
| Warmup 次数 | 1 |
| 测试次数 | 8 次 (512MB) / 4 次 (4GB) |
| Gateway 后端 | null backend（内存丢弃，排除 I/O 干扰） |
| CRC32C | **全部关闭**（client `send_crc32c=false`，gateway `VerifyCrc32c` 注释） |
| Buffer pool | 128（RDMA 默认从 16 扩大） |

### 2.2 测试命令

```bash
# RDMA
./build/bench/us3_turbo_access_rdma_multipart_bench \
  --endpoint=192.168.1.198:28080 \
  --object-size=536870912 --part-size=67108864 \
  --warmup=1 --count=8 --threads=8

# HTTP
./build/bench/us3_turbo_access_http_multipart_bench \
  --endpoint=192.168.1.198:28080 \
  --object-size=536870912 --part-size=67108864 \
  --warmup=1 --count=8 --threads=8

# GDS
./build/bench/us3_turbo_access_gds_multipart_bench \
  --endpoint=192.168.1.198:28080 \
  --object-size=536870912 --part-size=67108864 \
  --warmup=1 --count=8 --threads=8
```

---

## 三、性能测试结果

### 3.1 512 MB 对象（8 parts × 64 MB，8 并发）

| 指标 | RDMA | HTTP | GDS |
|------|------|------|-----|
| **吞吐量** | **18138 MiB/s** | 2246 MiB/s | 3092 MiB/s |
| 延迟 avg | **28.2 ms** | 227.9 ms | 165.6 ms |
| 延迟 p50 | **27.4 ms** | 228.4 ms | 163.1 ms |
| 延迟 p95 | **31.7 ms** | 242.3 ms | 174.0 ms |
| 延迟 p99 | **33.2 ms** | 254.2 ms | 174.3 ms |
| Client user CPU | 0.30 s | 1.41 s | 1.03 s |
| Client sys CPU | 0.07 s | 2.55 s | 0.42 s |
| Client CPU% | 165.9% | 216.9% | **109.2%** |
| 总耗时 | **0.23 s** | 1.82 s | 1.32 s |

**性能倍数（以 HTTP 为基准）：**

| 指标 | RDMA / HTTP | GDS / HTTP |
|------|-------------|------------|
| 吞吐 | **+708%** | +38% |
| 延迟 p50 | **8.3× 快** | 1.4× 快 |
| Client CPU | **-24%** | -50% |

### 3.2 4 GB 对象（32 parts × 128 MB，8 并发）

| 指标 | RDMA | HTTP | GDS |
|------|------|------|-----|
| **吞吐量** | 3272 MiB/s | 1581 MiB/s | **7766 MiB/s** |
| 延迟 avg | 1251.7 ms | 2591.1 ms | **527.4 ms** |
| 延迟 p50 | **244.1 ms** | 1736.5 ms | 527.1 ms |
| 延迟 p95 | 3707.0 ms | 4660.4 ms | **536.2 ms** |
| 延迟 p99 | 4190.7 ms | 5072.2 ms | **537.5 ms** |
| Client user CPU | 4.14 s | 7.82 s | **1.65 s** |
| Client sys CPU | 4.66 s | 14.83 s | **0.64 s** |
| Client CPU% | 175.8% | 218.6% | **108.6%** |
| 总耗时 | 5.01 s | 10.36 s | **2.11 s** |

**性能倍数（以 HTTP 为基准）：**

| 指标 | RDMA / HTTP | GDS / HTTP |
|------|-------------|------------|
| 吞吐 | +107% | **+391%** |
| 延迟 avg | 2.1× 快 | **4.9× 快** |
| Client CPU | -20% | **-50%** |

### 3.3 Gateway 资源消耗（512MB 测试序列）

| 测试阶段 | RSS (MB) | 增量 |
|---------|----------|------|
| 初始启动 | 91 MB | - |
| RDMA 测试后 | 847 MB | +756 MB |
| HTTP 测试后 | 1936 MB | +1089 MB |
| GDS 测试后 | 2903 MB | +967 MB |

> 内存增长主要来自各链路各自的 buffer pool：RDMA 预分配 128 × registered buffer（MR 长期注册），HTTP 和 GDS 在测试中逐步扩张 pool。

### 3.4 RDMA 优化前后对比

| 指标 | 优化前（有 CRC32C，pool=16） | 优化后（无 CRC32C，pool=128） | 提升 |
|------|------------------------------|-------------------------------|------|
| 吞吐（512MB，8并发） | 1117 MiB/s | **18138 MiB/s** | **16.2 倍** |
| 延迟 p50 | 434 ms | **27.4 ms** | **15.8 倍改善** |
| Client CPU% | 457% | **165.9%** | 节省 64% |

> 核心优化点：
> 1. **关闭 CRC32C**：Gateway 端 `VerifyCrc32c` + Client 端 `send_crc32c=false`
> 2. **Buffer pool 扩容**：`buffer_pool_max_idle` 从 16 → 128，减少 Acquire 锁竞争
> 3. **异步 commit**：CommitObject/CommitPart 改为 pending_commit 回调，消除 ~80ms cond_var 等待延迟

---

## 四、各链路特性总结

### RDMA (UCX RMA WRITE)

- **优势**：小对象（≤ 512MB）吞吐极高（18 GB/s，硬件利用率 85%），延迟最低（27ms）
- **劣势**：大对象（4GB）延迟抖动大（p50=244ms vs p99=4190ms），sys CPU 较高（4.66s）
- **适用**：延迟敏感场景、高频小对象、RDMA 网卡可用环境

### GDS (cuFile / GPUDirect Storage)

- **优势**：大对象吞吐最高（7.8 GB/s），CPU 消耗最低（108%），延迟稳定无抖动（p95/p99 紧贴 p50）
- **劣势**：小对象吞吐不及 RDMA，需要 NVIDIA GPU 环境
- **适用**：大对象批量传输、GPU 计算节点直传、CPU 资源受限场景

### HTTP (TCP)

- **优势**：通用，无硬件依赖，生态成熟
- **劣势**：吞吐最低，sys CPU 最高（大量 kernel TCP 拷贝），延迟最差
- **适用**：兼容性优先场景、无 RDMA/GPU 硬件环境

### 选型矩阵

| 场景 | 推荐链路 | 次选 |
|------|---------|------|
| 对象 < 1GB，延迟敏感 | **RDMA** | GDS |
| 对象 > 2GB，GPU 节点 | **GDS** | RDMA |
| 通用环境 / 无特殊硬件 | **HTTP** | - |
| CPU 严格受限 | **GDS** | RDMA |

---

## 五、与 Ceph 分片上传设计对比分析

### 5.1 Ceph 分片上传机制

Ceph 的对象存储接入层（RGW，RADOS Gateway）实现 S3 兼容的 Multipart Upload，其设计要点如下：

**控制面**

- `InitiateMultipartUpload`：在 RADOS 中创建上传状态对象（`{bucket}.multipart_shadow`），记录 upload_id
- `UploadPart`：每个 Part 写入独立的 RADOS 对象（命名为 `{key}.{upload_id}.{part_num}`），互不干扰
- `CompleteMultipartUpload`：合并各 Part 的 manifest，原子更新 bucket index，形成最终对象

**数据路径**

- RGW 作为**透明代理**：接收 HTTP 请求体 → 写入 RADOS OSD，无独立的 RDMA/GDS 数据面
- 每个 Part 写完立即持久化到 OSD，无内存 buffer，无 MR 注册
- 支持**并发写 Part**：多个客户端可同时上传不同 Part 号（S3 语义允许，Ceph 通过对象命名隔离）
- **内存开销可控**：无 pre-registered buffer，每 Part 走内核 TCP，无 RDMA MR 生命周期问题

**校验机制**

- MD5 校验：每个 Part 上传时计算 MD5，CompleteMultipartUpload 时校验 PartETag
- CRC32C：可选，作为 x-amz-checksum 头，RGW 会计算并回传

### 5.2 当前实现与 Ceph 设计对比

| 维度 | Ceph RGW | 当前实现（Us3TurboAccess） | 差距/问题 |
|------|----------|--------------------------|-----------|
| **数据面分离** | 无（RGW=proxy） | ✅ HTTP/RDMA/GDS 三条独立数据面，IDataPathExecutor 抽象 | 更灵活，支持零拷贝 |
| **Part 并发写** | ✅ 天然支持，对象命名隔离 | ✅ UploadParts(concurrency) 多线程并发 | 一致 |
| **Part 持久化** | ✅ 每 Part 写完即落盘 | ⚠️ null backend 模拟，生产需接真实后端 | 待生产接入 |
| **upload_id 管理** | RADOS 对象存储，持久化 | ✅ MultipartStore（内存，有 TTL） | 无持久化，重启丢失 |
| **Part 完整性** | MD5 per Part | ✅ CRC32C per Part（可关闭） | 更强（CRC32C 优于 MD5） |
| **MR 生命周期** | 无（TCP） | ⚠️ RDMA registered buffer 生命周期与 session 绑定 | 复杂，需关注泄漏 |
| **Buffer 预分配** | 无 | ✅ buffer_pool（RDMA/GDS），128 个 pre-registered buffer | 低延迟但内存固定占用 |
| **最大 Part 大小** | 5 GiB（S3 兼容） | max_msg_bytes 配置（当前 4 GB） | 接近兼容 |
| **最小 Part 大小** | 5 MiB（S3 兼容，最后一片除外） | ✅ min_part_size=5MiB 强制 | 符合 S3 语义 |
| **Part 数量** | 最多 10000 | 未见明确上限 | 需补充限制 |
| **乱序上传** | ✅ 支持（Part 号无序到达） | ✅ UploadParts 按 spec 分发，Complete 时排序 | 一致 |
| **断点续传** | ✅ upload_id 持久，重连可继续 | ❌ session TTL 到期即丢弃，重启需重传 | 不支持 |
| **Abort 清理** | ✅ AbortMultipartUpload 删除 shadow 对象 | ✅ AbortSession → backend.AbortUpload | 一致 |
| **并发 session 隔离** | RADOS 对象命名隔离 | ✅ session_id UUID 隔离，session_registry 分片锁 | 一致 |

### 5.3 当前设计合理性评估

#### ✅ 符合对象存储接入端设计原则的方面

1. **接口语义完整**：StartUpload / UploadPart / CompleteUpload / AbortUpload 四步语义与 S3 Multipart API 完全对齐，上层应用无需感知底层传输方式。

2. **传输路径正交**：IDataPathExecutor 抽象使 HTTP / RDMA / GDS 互不耦合，可按对象大小和硬件环境灵活选择，这比 Ceph RGW 的单一 TCP 路径更灵活。

3. **多线程并发 Part**：UploadParts 内部用线程池（concurrency 参数）并发上传多个 Part，性能线性扩展，设计合理。

4. **Buffer Pool 复用**：pre-registered buffer 避免每次 Part 都 `ibv_reg_mr`（Ceph 不需要是因为走 TCP），是 RDMA 场景正确的优化方向。

5. **控制面/数据面分离**：控制面走 brpc/protobuf（OpenSession / Commit），数据面走 UCX RMA WRITE，避免大数据走 RPC 框架，设计合理。

6. **Part 大小约束**：强制 min_part_size=5MiB（符合 S3 规范），gateway 端在 CommitPart 时检查，防止碎片化。

#### ⚠️ 需要关注的设计风险

1. **upload_id 无持久化**

   ```
   Ceph：upload_id → RADOS 持久对象，重启/故障后可继续
   当前：MultipartStore 纯内存 + TTL，gateway 重启 → 所有进行中 upload 丢失
   ```

   **风险**：生产环境 gateway 故障时，客户端已上传的所有 Part 数据丢失，需全量重传。  
   **建议**：MultipartStore 持久化到后端（或用 Redis/etcd），或在 Complete 时校验所有 Part 已持久化。

2. **RDMA Buffer 内存固定占用大**

   buffer_pool_max_idle=128，每个 buffer 最大 4GB，极端情况内存占用：128 × 对象大小。当前 512MB 测试后 RSS=847MB，4GB 对象理论上需要数百 GB 注册内存。  
   **建议**：按实际对象大小分级 pool（小对象 pool 和大对象 pool 分开），避免为小对象预留大 buffer。

3. **RDMA 大对象延迟抖动**

   4GB 测试中 p50=244ms 但 p99=4190ms（17倍抖动），而 GDS p99/p50 仅差 10ms。这说明 RDMA 路径在大对象 Part 并发时存在资源争抢（单 UCX worker、buffer pool 锁）。  
   **建议**：大对象路由到 GDS，小对象路由到 RDMA，在客户端 UploadParts 层实现自动选路。

4. **CRC32C 是性能开关而非可配置项**

   当前通过注释代码关闭 CRC32C，不应在生产中如此处理。  
   **建议**：通过 gateway flag `--ucx_enable_crc32c=false` 和 client option 控制，允许按场景配置（高速内网可关，跨机房必须开）。

5. **单 UCX ProgressLoop 线程**

   所有 RDMA 完成事件（RMA WRITE 完成、AM 处理）在单线程处理，限制高并发下吞吐。  
   **建议**（中期）：多 UCX Worker 架构，每个 worker 绑定一个 CPU core，理论再提升 2-4 倍。

6. **Part 数量无上限**

   Ceph RGW 对 S3 最大 10000 Parts 有检查，当前实现未见明确限制。  
   **建议**：在 StartUpload 或 UploadPart 时增加 part_number ≤ 10000 的校验。

### 5.4 总结

当前分片上传实现在**接口语义层面**与 S3 标准（Ceph RGW 等主流实现）保持对齐，控制面设计清晰，传输路径抽象合理，是一个**合理的对象存储接入端骨架**。

核心差距在于**生产可靠性**（upload_id 持久化、断点续传）和**性能调优完整性**（CRC32C 可配置、RDMA 大对象路由、单 worker 瓶颈）。这些是从 PoC 走向生产的必要补充，架构上无需推翻重来。

---

## 六、后续优化方向

### 短期（可立即实施）

| 优化 | 预期收益 | 难度 |
|------|---------|------|
| CRC32C 从代码注释改为 flag 控制 | 生产可配置 | 低 |
| Part 数量上限校验（≤ 10000） | S3 兼容性 | 低 |
| RDMA/GDS 自动选路（按对象大小） | 消除大对象 RDMA 抖动 | 中 |

### 中期（1-2 周）

| 优化 | 预期收益 | 难度 |
|------|---------|------|
| MultipartStore 持久化 | 支持断点续传，故障恢复 | 中 |
| Buffer pool 分级（小/大对象） | RDMA 内存占用降低 50%+ | 中 |
| 多 UCX Worker（per-core） | RDMA 吞吐再提升 2-4 倍 | 高 |

### 长期（生产接入）

| 优化 | 预期收益 | 难度 |
|------|---------|------|
| 接入真实后端（us3/ceph） | 端到端延迟实测 | 高 |
| CRC32C 硬件加速（AVX-512） | gateway 校验 CPU 降低 60%+ | 中 |
| 跨节点多 gateway 负载均衡 | 水平扩展到 100+ GB/s | 高 |
