# Bench 操作手册

## 目录结构

```
bench/
├── http_put_bench.cpp        # HTTP 单对象 PUT
├── http_multipart_bench.cpp  # HTTP 分段上传
├── rdma_put_bench.cpp        # RDMA/UCX 单对象 PUT
├── rdma_multipart_bench.cpp  # RDMA/UCX 分段上传
├── gds_put_bench_v2.cpp      # GDS(cuObject) 单对象 PUT  ← 当前版本
├── gds_multipart_bench.cpp   # GDS 分段上传
├── ucx_put_poc.cpp           # UCX 协议基线（独立，不依赖 client lib）
├── scripts/
│   ├── bench_compare.sh      # 三通路 × 多模式 × N 轮自动 runner
│   ├── http_bench.sh         # HTTP 单跑
│   └── sustained_bench.sh    # 稳态长跑
└── logs/                     # 输出 CSV + gateway 日志
```

## 编译产物

| 可执行文件 | 对应源文件 |
|-----------|-----------|
| `build/bench/us3_turbo_access_http_put_bench` | http_put_bench.cpp |
| `build/bench/us3_turbo_access_rdma_put_bench` | rdma_put_bench.cpp（RDMA/UCX 统一入口）|
| `build/bench/us3_turbo_access_gds_put_bench`  | gds_put_bench_v2.cpp |

## 通用参数（三条路径接口一致）

```
--endpoint   <host:port>    gateway brpc 地址（必填）
--bucket     <name>         目标 bucket（默认 us3-bench）
--threads    <N>            并发线程数（0 = hardware_concurrency/2）
--count      <N>            测量轮次（不含 warmup，默认 64）
--object-size <bytes>       单对象大小（默认 4MiB）
--warmup     <N>            warmup 轮次（不计入统计，默认 4）
--key-prefix <prefix>       对象 key 前缀
```

## 前置条件

### 1. Gateway 启动

根据要测试的通路，选择对应 flag 启动 gateway：

```bash
GW=./build/gateway/us3_turbo_access_gateway

# HTTP only
$GW --brpc_port=18082 --public_host=<IP> \
    --backend=memory --backend_capacity=$((128*1024*1024*1024)) \
    --gds_enable=false &

# HTTP + RDMA(UCX) + GDS（三路同时）
$GW --brpc_port=18082 --public_host=<IP> \
    --backend=memory --backend_capacity=$((128*1024*1024*1024)) \
    --ucx_enable=true  --ucx_port=18520 \
    --gds_enable=true  --gds_rdma_port=18516 &

# 等待就绪（grep "gateway ready"）
sleep 5
```

**RDMA/UCX 要求：**
- `--public_host` 必须是 RDMA NIC 的 IP（本机 `ibv_devices` 可查）
- gateway 支持 `--ucx_port`（默认 18520），client 自动发现

**GDS 要求：**
- nvidia-fabricmanager 运行中（`systemctl status nvidia-fabricmanager`）
- CUDA 可用（`nvidia-smi` 无报错）

### 2. 环境变量

```bash
# GDS bench 需要指定 GPU（多 GPU 时默认 device 0）
export CUDA_VISIBLE_DEVICES=0
```

---

## 快速 bench：三路 4MiB PUT

```bash
EP=192.168.1.198:18082
SIZE=4194304     # 4 MiB
COUNT=64
WARMUP=8

# HTTP
./build/bench/us3_turbo_access_http_put_bench \
    --endpoint=$EP --bucket=us3-bench \
    --threads=4 --count=$COUNT --object-size=$SIZE --warmup=$WARMUP

# RDMA/UCX
./build/bench/us3_turbo_access_rdma_put_bench \
    --endpoint=$EP --bucket=us3-bench \
    --threads=4 --count=$COUNT --object-size=$SIZE --warmup=$WARMUP

# GDS（需 GDS-enabled gateway）
./build/bench/us3_turbo_access_gds_put_bench \
    --endpoint=$EP --bucket=us3-bench \
    --threads=4 --count=$COUNT --object-size=$SIZE --warmup=$WARMUP
```

---

## 输出格式

每次 bench 输出两行：

```
# 人读摘要（stderr）
  threads=4 count=64 object_size=4194304 warmup=8 failed=0
  wall=0.159 s throughput=1606 MiB/s
  latency_ms: avg=9.8 p50=9.2 p95=13.9 p99=15.4

# JSON 结构化（stdout，可 jq 处理）
{"path":"rdma","mode":"put","threads":4,"count":64,"object_size":4194304,
 "warmup":8,"failed":0,"wall_s":0.159,"throughput_mbps":1606,"lat_avg_ms":9.8,
 "lat_p50_ms":9.2,"lat_p95_ms":13.9,"lat_p99_ms":15.4,
 "cpu_user_s":0.15,"cpu_sys_s":0.03,"cpu_pct":117.2}
```

收集 JSON 结果到 CSV：

```bash
for T in 1 4 8; do
  ./build/bench/us3_turbo_access_rdma_put_bench \
    --endpoint=$EP --threads=$T --count=64 --object-size=4194304 --warmup=8 \
    2>/dev/null >> bench/logs/result_$(date +%Y%m%d).jsonl
done
```

---

## 4MiB PUT 实测结果（loopback，memory backend）

> 测试时间：2026-06-04  
> 环境：mlx5_0（192.168.1.198），memory backend，`--count=64 --warmup=8`

### 吞吐量（MiB/s）

| 路径 | t=1 | t=4 | t=8 |
|------|-----|-----|-----|
| **GDS** | 563 | **5,811** | 5,332 |
| **RDMA/UCX** | 546 | **1,606** | 1,633 |
| **HTTP** | 109 | 792 | **1,969** |

### p50 延迟（ms）

| 路径 | t=1 | t=4 | t=8 |
|------|-----|-----|-----|
| **GDS** | 7.7 | **2.6** | 3.5 |
| **RDMA/UCX** | 8.4 | **9.2** | 17.7 |
| **HTTP** | 36.5 | 18.9 | **14.1** |

### 解读

- **GDS** 吞吐最高（GPU 零拷贝直连 backend），t=4 已达 5.8 GB/s；单线程延迟与 RDMA 相当（均 ~7ms）。
- **RDMA/UCX** t=4 达 1.6 GB/s，比 HTTP t=4 快 **2×**；延迟 p50=9ms（含 UCX TAG 握手 + S3 write）。
- **HTTP** 高并发（t=8）吞吐接近 2 GB/s，延迟随线程增加而降；单线程 109 MiB/s 受串行 RPC 限制。

---

## 批量对比脚本

```bash
# 三路 × {1,4,8} 线程 × 4MiB，输出到 CSV
EP=192.168.1.198:18082
OUT=bench/logs/compare_$(date +%Y%m%d_%H%M).csv
echo "path,threads,throughput_mbps,lat_p50_ms,failed" > $OUT

for PATH_NAME in http rdma gds; do
  BIN=./build/bench/us3_turbo_access_${PATH_NAME}_put_bench
  for T in 1 4 8; do
    JSON=$($BIN --endpoint=$EP --bucket=us3-bench \
      --threads=$T --count=64 --object-size=4194304 --warmup=8 2>/dev/null)
    TPUT=$(echo $JSON | python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(d['throughput_mbps'])")
    P50=$(echo  $JSON | python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(d['lat_p50_ms'])")
    FAIL=$(echo $JSON | python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(d['failed'])")
    echo "${PATH_NAME},${T},${TPUT},${P50},${FAIL}" >> $OUT
  done
done
cat $OUT
```

---

## 注意事项

1. **GDS bench** 需要 GPU device memory，`CUDA_VISIBLE_DEVICES` 未设时使用 device 0。
2. **RDMA/UCX bench** gateway 需要 `--ucx_enable=true` 和有效 RDMA NIC IP（`--public_host`）。
3. **memory backend** 数据不落盘，throughput 数字偏高；生产场景需换 S3 backend。
4. **UCX wakeup fd error** 日志（`Signaling wakeup failed: Bad file descriptor`）是 UCX client worker 析构时的已知无害噪音，不影响结果。
5. 重复测试前若 gateway 有 buffer 碎片（long run 后），重启 gateway 以获得稳定基线。
