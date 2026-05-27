# GDS 客户端通路重写：从 cuObjPut 回调到 RDMA token 直通

> 本文档记录 us3_turbo_access 客户端 GDS 通路（`client/src/transports/gds/`）的一次 in-place 重写。重写前 `Client::PutObject` 在 GDS 模式下走 cuObj SDK 的 `cuObjPut` 同步回调机制，单流吞吐 ~480 MiB/s、CPU·ms/MiB 达 1.80；重写后改走 `cuMemObjGetRDMAToken` 直通路径，单流吞吐 3.7 GB/s（峰值 5.6 GB/s）、CPU·ms/MiB 降到 0.30 —— 接近 RDMA 通路水平且不再被全局锁限并发。
>
> **关键认识**：GDS 旁路的是数据通路（GPU memory → BAR1 → NIC，不过 host RAM），但 cuObj SDK 的 `cuObjPut/cuObjGet` 这两个 API 内部本身是 CPU 密集 + 进程级共享态的。绕开它们直接调 SDK 的 token utility 函数才能真正发挥 GDS 优势。

| 字段 | 值 |
|---|---|
| 适用版本 | 2026-05 主线 |
| 改动范围 | `client/src/transports/gds/`、`client/include/.../client.h`、相关 examples |
| 不变量 | RDMA / HTTP 通路代码与行为零变更；gateway wire 零变更；`Client::PutObject`/`GetObject` 公共签名不变 |

## 一、性能 / CPU 对比

threads=4，2 轮均值，put 4 MiB×64，multipart 32 MiB×4 / part 8 MiB。

| path | mode      | 吞吐 (MiB/s) | p50 (ms) | p99 (ms) | CPU% | CPU·ms/MiB |
|------|-----------|-------------|----------|----------|------|-----------|
| **重写前 gds put** | 4 worker 同步 cuObjPut | 470  | 31.3 | 102 | 84.1 | **1.80** |
| **重写后 gds put** | 4 worker token 直通 | **3740** | **5.5** | 8.5 | 53.1 | **0.30** |
| (best round) | 4 worker token 直通 | 5644 | 2.7  | 3.9 | 73   | 0.18 |
| 对照 rdma put | host pinned + libibverbs | 925   | 145  | 277 | 27.9 | 0.31 |
| 对照 http put | brpc HTTP + IOBuf | 853   | 168  | 301 | 52.6 | 0.66 |

收益维度：

- **吞吐 ×7.9**（4 worker 平均）/ **×12**（best round）
- **p50 延迟 −82%**（31 ms → 5.5 ms）
- **CPU·ms/MiB −83%**（1.80 → 0.30），与 RDMA 持平
- 4 → 8 worker 进一步扩到 3.5 GB/s 后被 NIC 带宽饱和（说明已经触及硬件上限）

## 二、改动前的流程（cuObjPut 回调机制）

### 2.1 调用图

```
Client::PutObject(req, buffer)
   │
   ▼
TransferRouter::PutObject  ────►  GdsTransferPath::ExecutePut
                                         │
                                         ▼
                                  ┌──────────────────────────────────────┐
                                  │ CuObjectClient::ExecuteTransfer       │
                                  │                                       │
                                  │  1. 全局 std::mutex g_cuobj_global_mu │
                                  │     std::scoped_lock                  │
                                  │  2. 构造 cuObjClient 实例（每次新建） │
                                  │  3. cuMemObjGetDescriptor(buf,sz)    │
                                  │     —— pin GPU 页到 BAR1, ms 级       │
                                  │  4. ChunkDispatcher 准备 RPC ctx     │
                                  │  5. 同步调 cuObjPut(client, ctx,     │
                                  │              buf, sz)                 │
                                  │                                       │
                                  │     SDK 内部按 chunk 切，每个 chunk  │
                                  │     回调 ────► CuObjPutCallback       │
                                  │                       │               │
                                  │                       ▼               │
                                  │              dispatcher.Dispatch      │
                                  │                       │               │
                                  │                       ▼ baidu_std     │
                                  │              GdsChunk RPC 同步等响应 │
                                  │                       │               │
                                  │                       └──返回 size──► │
                                  │                                       │
                                  │  6. cuMemObjPutDescriptor(buf)        │
                                  │  7. ~cuObjClient                      │
                                  │  8. unlock g_cuobj_global_mu          │
                                  └──────────────────────────────────────┘
```

### 2.2 路径上的 7 个开销点

| # | 操作 | 频次 | 单次成本 | 备注 |
|---|------|------|---------|------|
| P1 | `cuMemObjGetDescriptor` | per PUT | ~ms 级 (nvidia_p2p_get_pages syscall) | NVIDIA Best Practices 文档明确点名为 *IO Pattern 1（anti-pattern）* |
| P2 | `cuObjClient` 构造 / 析构 | per PUT | 多份内部表分配、ops 表拷贝 | |
| P3 | `g_cuobj_global_mu` 全局锁 | per PUT | 阻塞等待 + 内核态等待 | N 个 worker 完全串行化 |
| P4 | 同步 `cuObjPut` 数据面调用 | per PUT | SDK 内 chunk 循环 + 同步 polling | SDK 不暴露 async 接口 |
| P5 | callback 内同步 `GdsChunk` RPC | per chunk | 1 RTT brpc 来回 | 4 MiB 单 PUT 通常 1 个 chunk，但仍是同步等 |
| P6 | `cuMemObjPutDescriptor` | per PUT | unpin BAR1 | 与 P1 对称 |
| P7 | 缺失：buffer 复用上下文 | per PUT | 用户每次都"新"上传 | descriptor 表用完即丢 |

### 2.3 这些为什么不是 GPUDirect Storage 应有的形态

NVIDIA 官方 [Best Practices Guide](https://docs.nvidia.com/gpudirect-storage/best-practices-guide/index.html) 明确指出：

> *"BufRegister and cuFileBufDeregister are continuously issued in the loop"* 是反模式。**register 一次、reuse 多次**才是正确用法。Pin 一次内存到 BAR1 是 ms 级开销，叠加在每次 IO 上完全淹没了零拷贝数据面的优势。

而且 `cuObjPut` 这个 API 本身（从 PoC 实测）在我们的 SDK 版本里**进程内多线程并发不安全**——单 cuObjClient 并发崩；per-thread cuObjClient 并发也崩。这点在 NVIDIA 头文件里没明确写，是实测发现的。所以即使把 P1 / P2 优化掉，P3 全局锁也仍然必须保留，单流上限就被锁死。

### 2.4 PoC 摸底（保留作为重写决策依据）

用环境变量切换 5 种实现方式，跑同样的 `gds_put_bench_v2 --threads=4 --count=64`：

| Mode | 配置 | 吞吐 (MiB/s) | 结论 |
|------|------|-------------|------|
| 0 | 原版 | 470 | baseline |
| 1 | 缓存 descriptor，仍 cuObjPut + 锁 | 1788 | 证明 P1 是大头 |
| 2 | 缓存 descriptor + 单例 client + 锁 | 1892 | 单流极限 ≈ 1.9 GB/s |
| 3 | 单例 client，**去锁** | **崩** | cuObjPut 不能并发 |
| 4 | per-thread client，**去锁** | **崩** | 多 client 也不能并发 |

结论：只要继续走 `cuObjPut`，进程级锁就不能去；上限锁定 ~1.9 GB/s。要突破必须绕开 `cuObjPut`。

## 三、改动后的流程（RDMA token 直通）

### 3.1 关键发现

`libcuobjclient.so` 导出符号里，除了 `cuObjPut/cuObjGet`，还有一对**直通 token API**：

```cpp
// 来自 /usr/local/cuda-13.1/.../include/cuobjclient.h
cuObjErr_t cuMemObjGetRDMAToken(void *ptr, size_t size, size_t buffer_offset,
                                 cuObjOpType_t operation, char **desc_str_out);
cuObjErr_t cuMemObjPutRDMAToken(char *desc_str);
```

`cuMemObjGetRDMAToken` 是一个**纯 utility 调用**：给一段已 register 过的 device buffer，生成 `(offset, size, op)` 的 RDMA token 字符串。这个 token 拿到之后，**数据怎么走完全是调用方的责任** —— 没有 SDK 内部状态机、没有 chunk 回调、没有同步等待。

micro-PoC `examples/gds_token_probe.cpp` 验证了：
- **Q5**：4 线程 × 10 000 次并发 `GetRDMAToken/PutRDMAToken` 在共享 client 上 **40 000 / 40 000 成功**。Token 生成线程安全。
- **Q6**：故意泄漏 1 个 token + 不 unregister 直接进程退出 → 无 nvidia-fs 死锁告警。失败路径不会被 token 泄漏污染。
- 单次 GetRDMAToken 开销 ~80 μs（4 MiB PUT 的 1%，可忽略）。

### 3.2 新调用图

```
Client::PutObject(req, buffer)
   │
   ▼
TransferRouter::PutObject  ────►  GdsTransferPath::ExecutePut
                                         │
                                         ▼
                                  ┌─────────────────────────────────────┐
                                  │ CuObjectClient::ExecuteTransfer      │
                                  │  (整体重写：< 50 行)                  │
                                  │                                       │
                                  │  1. GdsMemoryRegistry::AcquireToken  │
                                  │     │                                 │
                                  │     ▼                                 │
                                  │     如果 buffer 没注册 → lazy        │
                                  │     register（短锁串行，一次性）     │
                                  │     ▼                                 │
                                  │     cuMemObjGetRDMAToken (无锁)      │
                                  │     ▼                                 │
                                  │     RAII GdsRdmaToken 句柄           │
                                  │                                       │
                                  │  2. ChunkDispatcher::Dispatch        │
                                  │     │                                 │
                                  │     ▼ baidu_std                      │
                                  │     一发 GdsChunk RPC 同步等响应    │
                                  │     ────► gateway 端 libibverbs RDMA │
                                  │                                       │
                                  │  3. GdsRdmaToken 析构 → PutRDMAToken │
                                  └─────────────────────────────────────┘
```

整条数据路径**无全局锁**，token 生成自身线程安全，每个 worker 各发各的 RPC。

### 3.3 状态布局

```
┌───────────── 进程级单例（CuObjState） ─────────────────────────┐
│                                                                 │
│  cuObjClient 实例（一份）  ← lazy 初始化，atexit destruct      │
│  ┌─ ops 表（put/get 桩函数，不会被触发）                       │
│  └─ alignas(alignof(cuObjClient)) storage                       │
│                                                                 │
│  registration_mu_  ─── 仅守护下面的 map（短锁）                │
│  registered_  ──── unordered_map<void*, size_t>                │
│       │                                                         │
│       └── { ptr_A, size_A }  ← 用户 RegisterDeviceBuffer 录入   │
│           { ptr_B, size_B }                                     │
│           ...                                                   │
│                                                                 │
│  析构时遍历 registered_ 调 PutDescriptor（best-effort 保护）    │
└─────────────────────────────────────────────────────────────────┘
```

- **process-wide singleton**：descriptor 表本来就是 process-wide（PoC mode 1 证实），起多份 cuObjClient 没意义还消耗资源
- **`registration_mu_` 只在 register/unregister 时短暂持有**：数据面 `AcquireToken` 只在第一次见到 ptr 时走 lazy 路径，命中后无锁
- **`cuMemObjGetRDMAToken` 自身线程安全**（PoC Q5 验证）

### 3.4 用户契约（新增公开 API）

```cpp
class Client {
 public:
  // 推荐：cudaMalloc 后立即注册；idempotent
  [[nodiscard]] Result<bool> RegisterDeviceBuffer(void* ptr, std::size_t size);

  // 必须：cudaFree 之前调用（不调可能触发 nvidia-fs 内核死锁）
  [[nodiscard]] Result<bool> UnregisterDeviceBuffer(void* ptr);
};
```

- 非 GDS 通路（HTTP / RDMA）下两个调用 no-op 返 success，便于上层无 if 切换
- 没调 RegisterDeviceBuffer 时第一次 PUT 会 lazy register；功能正确但首笔有 ms 级抖动
- 双注册同一 ptr → idempotent，第二次直接 success
- Client 析构时遍历未 unregister 的 buffer 走 PutDescriptor 保险，但用户**仍应自己显式 unregister**（best-effort 不算契约）

## 四、原理详解

### 4.1 GDS 数据流（不变）

```
┌─ GPU memory (cudaMalloc) ─┐                       ┌─ Gateway host ─┐
│                            │                       │                │
│   user buffer              │                       │                │
│        ↓ cuMemObjGetDesc   │                       │                │
│   pinned to BAR1 region    │ ── RDMA write ──►     │  libibverbs MR │
│                            │   (NIC DMA reads      │  receives data │
│                            │    from BAR1 directly)│        ↓       │
│                            │                       │  CompositeBackend
└────────────────────────────┘                       └────────────────┘
```

注意：数据不过 host RAM，CPU 不参与拷贝。这是 GPUDirect Storage 的核心。我们的重写**没改变**这一点。

### 4.2 改动改变了什么

只改了"控制路径如何告诉对端来取数据"这一段：

**改前**：调用 `cuObjPut` → SDK 内部串行化 → SDK 回调 `CuObjPutCallback`（带 token 参数）→ 我们在 callback 里发 RPC → RPC 响应回来 SDK 知道 chunk 完成 → SDK 收尾 → `cuObjPut` 返回。整段被 `g_cuobj_global_mu` 串行化。

**改后**：直接调 `cuMemObjGetRDMAToken` 拿 token（这是个无副作用的 utility 调用，不会触发任何数据传输）→ 我们自己发 RPC 给 gateway，把 token 塞过去 → gateway 用 token 跟 GPU 协商建 libibverbs RDMA write 通道 → 数据从 BAR1 直接 DMA 到 gateway → gateway RPC 响应 → 客户端释放 token。

两条路径**数据面行为完全一致**（同一个 BAR1 region、同一种 RDMA op、同一个 gateway 端代码），只是控制面驱动方式不同。

### 4.3 为什么 token 直通能去掉全局锁

回到 PoC 结论：

| 测试 | 是否安全 |
|------|---------|
| 单 cuObjClient + 并发 `cuObjPut` | 不安全（崩） |
| 多 cuObjClient + 并发 `cuObjPut` | 不安全（崩） |
| 单 cuObjClient + 并发 `cuMemObjGetRDMAToken` | **安全**（4 线程 × 10 000 iter 零失败） |

差别在于 `cuObjPut` 维护了 SDK 内部的 in-flight 状态机（RDMA QP / CQ / chunk 队列），并发调用就死锁。而 `cuMemObjGetRDMAToken` 只是查表 + 分配字符串，完全可重入。

**这意味着**：去掉 cuObjPut → 去掉它要求的串行 → 多 worker 真正能并发 → 单 GPU 出 BAR1 的能力直接被打开。

### 4.4 为什么 CPU 占用还降这么多

旧路径的 CPU 主要烧在：

1. `nvidia_p2p_get_pages` syscall（每 PUT 都跑）—— 用户态/内核态切换 + 页表操作
2. `cuObjClient` 构造时的内部表分配 / ops 表拷贝
3. SDK 内部 chunk 调度的轮询
4. 全局锁的内核态等待（被 lock 的线程进入 futex_wait）

新路径只有：

1. 一次性的 `RegisterDeviceBuffer`（不在 IO 热路径）
2. `cuMemObjGetRDMAToken`（80 μs 级别的字符串生成）
3. brpc RPC 本身

第 1 项摊销到 N 次 IO 后趋零；第 2/3 项是必要开销；第 4 项消失。所以 CPU·ms/MiB 从 1.80 砸到 0.30。

### 4.5 错误路径

| 场景 | 行为 |
|------|------|
| AcquireToken 失败（注册失败 / SDK 错误） | 返 retryable Error；上层 retry wrapper 接管 |
| RPC 失败 | token RAII 在析构时正常 release；不泄漏 |
| 进程崩溃 | token 进程退出全释放；nvidia-fs 不会死锁（Q6 验证） |
| 用户 cudaFree 时未 unregister | Client 析构时 best-effort PutDescriptor；用户 free 之前我们已 unregister 则正常 |
| 双 Unregister 同一 ptr | idempotent，第二次 no-op success |

### 4.6 与 Gateway 的协作

**Gateway wire 零变更**。`GdsChunk` proto 仍接受 `rdma_token` 字段，无论 token 是从老的 cuObjPut 回调来还是从新的 GetRDMAToken 来，对 gateway 而言是同一个字符串。这意味着：

- 旧版 client + 新版 gateway 兼容
- 新版 client + 旧版 gateway 兼容
- 滚动升级零中断

## 五、改动文件清单

```
新增：
  examples/gds_token_probe.cpp                          (micro-PoC, Q5/Q6 验证)

整体重写：
  client/src/transports/gds/cuobject_client.cpp        (~310 行 → ~110 行)
  client/src/transports/gds/gds_memory_registry.h
  client/src/transports/gds/gds_memory_registry.cpp    (V1 stub → singleton state)

接口扩展：
  client/include/us3_turbo_access/client/client.h      (+ Register/Unregister)
  client/src/core/client/client.cpp                    (+ 实现)
  client/src/transports/gds/cuobj_library.h           (+ GetRDMAToken/PutRDMAToken sig)
  client/src/transports/gds/cuobj_library.cpp         (+ 符号绑定)

仅注释更新（行为不变）：
  client/src/transports/gds/chunk_dispatcher.{h,cpp}

bench/example 配合：
  examples/gds_gateway_example.cpp
  examples/gds_put_bench_v2.cpp
  examples/gds_multipart_bench.cpp
  examples/CMakeLists.txt
```

**删除的代码**：

- `CuObjClientHandle` RAII 容器
- `CuObjDescriptorGuard` RAII 容器
- `CuObjCallbackContext` 回调上下文
- `CuObjGetCallback` / `CuObjPutCallback` 静态回调函数
- `ExecuteControlCallback` 桥接函数
- `RdmaTokenFromInfo` / `ResolveCallbackContext` helper
- `g_cuobj_global_mu` 进程级全局锁
- PoC 阶段加的 `US3_GDS_POC_MODE` 5-mode 切换代码全删

## 六、回归验证

| 测试 | 命令 | 结果 |
|------|------|------|
| 单 PUT 正确性 | `gds_gateway_example 192.168.1.198:18082 4194304 us3-bench k` | PUT+HEAD+GET memcmp same |
| RDMA 通路不退化 | `bash scripts/rdma_test.sh` | PUT / async / multipart 全过 |
| HTTP 通路不退化 | `bash scripts/http_test.sh` | verify example 全过 |
| GDS put 性能 | `gds_put_bench_v2 --threads=4 --count=64` | 3740 MiB/s (avg) |
| GDS multipart 性能 | `gds_multipart_bench --threads=4 --count=4` | 466 MiB/s |
| 跨通路对比 | `scripts/bench_compare.sh --rounds 2` | 12 行 CSV 完整 |
| Token API PoC | `gds_token_probe --threads=4 --iters=10000` | 40 000 / 40 000 ok |

## 七、未来工作

本次重写聚焦"绕开 cuObjPut"这一层，下面这些后续优化与本次设计**正交**，可独立推进：

1. **控制面前置**：当前 `GdsChunk` RPC 仍按 chunk 同步发；4 MiB PUT 通常 1 chunk 不影响，但大对象（> 16 MiB）或细分 chunk 时多 RPC 会回到 N×RTT。可以加 batched `BeginGdsPut(expect_chunks=N)` 一次性颁多个 token。需要动 proto。
2. **buffer 池自动管理**：目前要求用户配对 Register/Unregister；可以提供 `Client::AllocateDeviceBuffer(size)` 一体化分配/注册/RAII 释放。
3. **GET 路径压测补齐**：本轮主要测了 PUT；GET 走相同 token 通路但需要等服务器 DMA 写完 → 客户端回调，特性可能不同。
4. **Multipart 加速**：multipart 当前 466 MiB/s 远未饱和。瓶颈是 4 个 part 串行 + control plane RPC 排队，与 token 路径无关，需要在 `MultipartUpload::UploadParts` 层加 in-flight 流水。

## 八、关键引用

- NVIDIA GPUDirect Storage Best Practices Guide：<https://docs.nvidia.com/gpudirect-storage/best-practices-guide/index.html>
- NVIDIA cuObject Client API：`/usr/local/cuda-*/targets/x86_64-linux/include/cuobjclient.h`
- 本仓库相关代码：
  - `client/src/transports/gds/cuobject_client.cpp` — 新数据面入口
  - `client/src/transports/gds/gds_memory_registry.cpp` — 单例 state + token 工厂
  - `examples/gds_token_probe.cpp` — PoC 验证脚本
