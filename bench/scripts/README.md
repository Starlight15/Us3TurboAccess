# Bench 脚本使用手册

## 脚本一览

| 脚本 | 用途 |
|------|------|
| `bench_compare.sh` | **主入口**：自动重启 gateway，按 gds → rdma → http 顺序跑 put + multipart，输出 CSV |
| `sustained_bench.sh` | 多轮持续测试，用于观察 round-to-round 抖动 |
| `http_bench.sh` | 单独跑 HTTP PUT，快速验证 HTTP 通路 |

---

## bench_compare.sh —— 三路分段上传测试

### 工作流程

```
kill 已有 gateway
  ↓
GDS 路径：启动 gds_enable=true gateway → 跑 gds:put + gds:multipart × N 轮
  ↓
RDMA 路径：重启 gateway（ucx_enable=true）→ 跑 rdma:put + rdma:multipart × N 轮
  ↓
HTTP 路径：重启 gateway（最小配置）→ 跑 http:put + http:multipart × N 轮
  ↓
写 CSV，打印汇总
```

每条 path 独立持有一个 gateway 实例，跑完自动 stop，再起下一个——避免三路 endpoint 互相干扰。

### 快速上手

```bash
cd /mnt/us3_test/xinghui.shao/gds/Us3TurboAccess

# 仅跑 multipart，三路各 2 轮
bash bench/scripts/bench_compare.sh \
  --paths gds,rdma,http \
  --modes multipart \
  --rounds 2

# 三路 put + multipart 各 1 轮（默认）
bash bench/scripts/bench_compare.sh

# 自定义对象大小 + 线程数
bash bench/scripts/bench_compare.sh \
  --modes multipart \
  --mp-object-size $((64*1024*1024)) \
  --mp-part-size $((16*1024*1024)) \
  --threads 8 \
  --rounds 3 \
  --out bench/logs/my_run.csv
```

### 完整参数

```
--paths LIST          通路列表，逗号分隔（默认 rdma,gds,http）
--modes LIST          模式列表，逗号分隔（默认 put,multipart）
--rounds N            每个 bench 跑 N 轮（默认 2）
--threads N           并发线程数（默认 4）

# PUT 模式参数
--object-size N       单对象大小，字节（默认 4 MiB）
--put-count N         测量笔数（默认 64）
--put-warmup N        warmup 笔数（默认 8）

# Multipart 模式参数
--mp-object-size N    单对象总大小，字节（默认 32 MiB）
--mp-part-size N      每个 part 大小，字节（默认 8 MiB，不能小于 5 MiB）
--mp-count N          测量轮数（默认 4）
--mp-warmup N         warmup 轮数（默认 1）

# 其他
--out FILE            CSV 输出路径（默认 bench/logs/bench_results.csv）
--cpus CPULIST        绑核，例如 "0-3"（需要 taskset）
```

### 环境变量（覆盖 gateway 地址）

```bash
export PUBLIC_HOST=192.168.1.198   # RDMA NIC IP（默认值）
export BRPC_PORT=18082             # gateway brpc 端口（默认值）
export UCX_PORT=18520              # RDMA/UCX 监听端口（默认值）
export GDS_RDMA_PORT=18516         # GDS cuObjServer 端口（默认值）
export BACKEND_CAPACITY=$((8*1024*1024*1024))  # memory backend 容量（默认 8 GiB）
```

### 输出格式

**实时进度（stderr）：**
```
[06:49:56] starting gateway rdma=false gds=true ...
[06:49:59] gateway ready pid=786999
[06:49:59] run gds:multipart round=1
[06:50:12] done. CSV: bench/logs/bench_results.csv
```

**CSV 内容（bench/logs/bench_results.csv）：**
```
round,path,mode,threads,object_size,count,throughput_mbps,lat_p50_ms,lat_p95_ms,lat_p99_ms,...
1,gds,multipart,4,33554432,8,415.763,82.333,90.654,90.706,...
1,rdma,multipart,4,33554432,8,469.131,67.656,90.706,90.881,...
1,http,multipart,4,33554432,8,211.211,148.165,183.068,187.181,...
```

---

## sustained_bench.sh —— 多轮稳态测试

用于观察长时间运行下各通路的吞吐抖动（round-to-round jitter）。每个 cell 跑多轮，结果带 round 编号，适合用 Excel / pandas 分析。

```bash
# 默认 5 轮，结果写 bench/logs/sustained_results.csv
bash bench/scripts/sustained_bench.sh

# 自定义轮数和输出
bash bench/scripts/sustained_bench.sh \
  --rounds 10 \
  --out bench/logs/sustained_$(date +%Y%m%d).csv
```

内置测试矩阵（每条 path 各跑 put + multipart）：

| path | mode | count | threads | object_size | part_size |
|------|------|-------|---------|-------------|-----------|
| rdma | put | 8192 | 4 | 4 MiB | — |
| gds | put | 32768 | 4 | 4 MiB | — |
| http | put | 8192 | 4 | 4 MiB | — |
| rdma | multipart | 512 | 4 | 32 MiB | 8 MiB |
| gds | multipart | 512 | 4 | 32 MiB | 8 MiB |
| http | multipart | 384 | 4 | 32 MiB | 8 MiB |

---

## http_bench.sh —— HTTP 单路快速验证

```bash
# 快速跑 HTTP PUT
bash bench/scripts/http_bench.sh --threads 4 --count 128 --warmup 8

# 绑核（bench 进程限制在 CPU 0-3）
bash bench/scripts/http_bench.sh --cpus "0-3" --threads 4
```

---

## 前置条件

**RDMA 通路：**
- `PUBLIC_HOST` 必须设为 RDMA NIC 的 IP（`ibv_devices` 可查）
- UCX 库可用

**GDS 通路：**
- `nvidia-fabricmanager` 运行中（`systemctl status nvidia-fabricmanager`）
- `nvidia-smi` 无报错

**通用：**
- `python3` 在 PATH 中（用于 JSON → CSV 转换）
- gateway 二进制已编译：`cmake --build build --target us3_turbo_access_gateway`
- bench 二进制已编译：`cmake --build build --target us3_turbo_access_http_multipart_bench us3_turbo_access_rdma_multipart_bench us3_turbo_access_gds_multipart_bench` 等

---

## 常见问题

**Q: UCX wakeup fd error 是什么？**

```
UCX ERROR Signaling wakeup failed: Bad file descriptor
```

UCX client worker 析构时的已知无害噪音，写到 stderr，不影响测试结果，可忽略。

**Q: gateway not ready 超时怎么办？**

查看 `bench/logs/gateway_bench_18082.log`，常见原因：
- 端口被占用（上一轮 gateway 未退出）：`pkill -f us3_turbo_access_gateway`
- GDS 路径时 `nvidia-fabricmanager` 未启动

**Q: 如何只跑一条通路？**

```bash
bash bench/scripts/bench_compare.sh --paths rdma --modes multipart --rounds 2
```

**Q: CSV 为空但 bench 输出正常？**

确认 `python3` 可用。脚本用 python3 做 JSON→CSV 转换。
