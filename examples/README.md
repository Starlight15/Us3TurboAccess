# examples/

功能验证 / 回归用例 + 环境探测工具。**性能压测代码不在这里**，见 `../bench/`。

## 目录

```
examples/
├── *_example.cpp          # 单通路 / 跨通路 功能示例
├── probe/                 # 环境 / 设备探测工具（CUDA、RDMA、cuObj、token）
├── scripts/               # 功能测试脚本（HTTP / RDMA / multipart / P0 case 等）
└── logs/                  # 运行日志输出（.gitignore'd）
```

## 编译产物

- 由 `examples/CMakeLists.txt` 描述
- 输出位置 `build/examples/us3_turbo_access_<name>_{example,probe}`

## 常用脚本

| 脚本 | 用途 |
|------|------|
| `scripts/http_test.sh` | 端到端 HTTP 通路 PUT / GET / verify |
| `scripts/rdma_test.sh` | 端到端 RDMA 通路 PUT / async / multipart |
| `scripts/multipart_test.sh` | 三通路 multipart 流程 |
| `scripts/test_p0_2.sh` | P0 项目级回归 (8 MiB + 12 MiB 边界) |
| `scripts/test_413.sh` | HTTP PUT 超 size 拒绝 |
| `scripts/test_backend.sh` | Backend 写入 / 读取一致性 |
| `scripts/test_bvar.sh` | Gateway bvar 输出格式 |
| `scripts/test_crc.sh` | CRC32C 端到端 |

脚本默认从 `${US3_REPO_ROOT}` 推导仓库根（脚本上溯 `../../`），可被环境变量覆盖：

```bash
US3_REPO_ROOT=$(pwd) ./examples/scripts/http_test.sh
```

输出 log 落 `examples/logs/`。
