# RDMA READ 模式压测结果

## 测试环境
- Gateway: 192.168.1.198:28080 (UCX 18521)
- Backend: null (内存丢弃)
- 网络: loopback

## 当前实现（RMA READ）
- 架构: Server 从 Client 读数据 (`ucp_get_nbx`)
- Client: 单 worker，所有线程共享
- 连接: per-session 短连接（每次创建/关闭 ep）

## 性能数据

### 4 线程 × 100 对象 × 4MB
```
threads=4 count=100 object_size=4194304 failed=0
wall=10.31s throughput=38.8 MiB/s
latency: p50=17.6ms p95=18.2ms p99=5182ms
```

### 8 线程 × 100 对象 × 4MB
```
threads=8 count=100 object_size=4194304 failed=0  
wall=10.50s throughput=38.1 MiB/s
latency: p50=17.7ms p95=5134ms p99=10429ms
```

### 4 线程 × 50 对象 × 16MB
```
threads=4 count=50 object_size=16777216 failed=0
wall=5.20s throughput=154 MiB/s
latency: p50=35.5ms p95=2859ms p99=5183ms
```

## 对比 UCX PoC（RDMA WRITE）
| 指标 | RMA READ（当前） | RMA WRITE（PoC） | 差距 |
|------|-----------------|------------------|------|
| 吞吐 4MB | 38.8 MiB/s | 19741 MiB/s | **509×** |
| p50 延迟 | 17.6 ms | 0.203 ms | 87× |

## 性能瓶颈分析
1. **单 worker 竞争**: 所有线程共享一个 `ucp_worker`，progress 互斥
2. **Per-session ep**: 每次 PUT 都创建/关闭连接（p99 5s+）
3. **READ 语义**: Server 主动 GET 变成同步操作，无法流水线
4. **AM 握手**: 每次都需要 AM_REGISTER 建立 ep_ready

## 建议
切换到 **RDMA WRITE 模式**（`ucp_put_nbx`），预期性能提升 500× 以上。
