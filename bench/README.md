# bench/

性能压测代码。**功能验证 / 回归不在这里**，见 `../examples/`。

## 目录

```
bench/
├── *_bench.cpp            # 各通路 PUT / multipart bench 代码
├── scripts/               # 压测脚本（多轮 / 多通路 / 多模式 runner）
└── logs/                  # 压测日志 + CSV 输出
```

## 编译产物

- 由 `bench/CMakeLists.txt` 描述
- 输出位置 `build/bench/us3_turbo_access_<path>_<mode>_bench`

## 主要脚本

| 脚本 | 用途 |
|------|------|
| `scripts/bench_compare.sh` | 三通路 × {put,multipart} × N 轮 runner，CSV 收集 |
| `scripts/http_bench.sh` | HTTP put bench 单跑 |
| `scripts/sustained_bench.sh` | 长跑稳态压测 |

脚本默认从 `${US3_REPO_ROOT}` 推导仓库根（脚本上溯 `../../`），可被环境变量覆盖：

```bash
./bench/scripts/bench_compare.sh --modes put --rounds 3 \
    --threads 4 --object-size $((4*1024*1024)) \
    --put-count 64 --put-warmup 8
```

## 输出位置

| 文件 | 说明 |
|------|------|
| `logs/gateway_bench_<port>.log` | 各轮 gateway 日志 |
| `logs/bench_results.csv` | bench_compare.sh 默认 CSV 输出 |
| `logs/sustained_results.csv` | sustained_bench.sh 默认 CSV |
| `logs/bench_put_*.csv` | 历史 baseline（已 git track） |

新生成的 `.log` 默认 .gitignore；`logs/*.csv` 可手动 `git add` 保留为 baseline。
