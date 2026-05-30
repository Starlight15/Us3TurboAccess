# Phase 3.3 决策：暂不做 ClientCore 拆分

## 现状回顾

ClientCore 当前持有：
- ChannelRegistry（P3.2 引入，三个 baidu_std client 共享 channel）
- MetadataClient / GdsDataClient / RdmaDataPlaneClient / HttpDataClient（4 个 RPC client）
- GdsMemoryRegistry / CuObjectClient（GDS 专属）
- GdsTransferPath / RdmaTransferPath / HttpTransferPath（三通路）
- TransferRouter / UploadCoordinator / ClientExecutor

## 决策

**P3.3（拆 HttpBundle/RdmaBundle/GdsBundle）暂不做。**

## 理由

1. **目标价值未触发**
   - 拆 bundle 是为了"扩第四种通路时不改 ClientCore"
   - 当前没有第四通路计划
   - 目标更像是"代码组织美化"而非功能驱动

2. **P3.2 已缓解主要耦合**
   - 三个 baidu_std client 的 channel 已经统一到 ChannelRegistry
   - 这是之前最重的耦合点；拆 bundle 在此基础上的边际收益有限

3. **工作量 3 天，无明确性能/正确性收益**
   - 三阶段 8 项中已完成 7 项核心改造
   - 整体已经实现：超时统一、重试统一、GET CRC、进度回调、并发 GET、metrics、channel 复用
   - 当前应优先把已有改造的真实场景跑充分

4. **保持稳定性**
   - 当前 ClientCore::Impl 经历过多轮 review，结构稳定
   - 拆 bundle 会动 ~10 个文件，回归测试成本不低

## 触发条件（什么时候做）

满足以下任一条件时再启动 P3.3：

- 引入第四种通路（如 NVMe-oF / S3 兼容 / TLS HTTP/2）
- ClientCore::Impl 成员超过 18 个，构造列表难读
- 出现明确的 GDS 专属泄漏（如 RegisterDeviceBuffer 之外又新增 GDS-specific API）

## 已完成的 8 项改造汇总

| 阶段 | 任务 | Commit | 价值 |
|------|------|--------|------|
| P1.1 | 三通路超时 | 5e51287 | 防止 worker 卡死 |
| P1.2 | RDMA/GDS 重试 | bf26405 | 与 HTTP 对齐 |
| P1.3 | GDS GET CRC | f535eb1 | 数据完整性 |
| P2.1 | 进度回调 | e6d434a | 大对象用户体验 |
| P2.2 | GDS 并发分片 GET | 062c291 | 大对象 GET 性能 |
| P3.1 | ClientMetrics | b1c4ef3 | client 可观测性 |
| P3.2 | Channel 复用 | 0339d84 | 资源开销 ↓ 50% |
| P3.3 | ClientCore 拆分 | **延后** | 见上 |

## 跨通路对齐验证

| 能力 | HTTP | RDMA | GDS | 状态 |
|------|------|------|-----|------|
| 端到端超时 (request_timeout) | ✅ | ✅ | ✅ | 对齐 |
| 失败自动重试 | ✅ | ✅ | ✅ | 对齐 |
| GET CRC 校验 | ✅ | ⚠️ GET 未实现 | ✅ | 当前最大对齐 |
| PUT/UploadPart CRC | ✅ | ✅ | ✅ | 对齐 |
| 进度回调 | ✅ | ✅ | ✅ | 对齐 |
| 并发分片 GET | ✅ | ⚠️ GET 未实现 | ✅ | 当前最大对齐 |
| Client metrics | ✅ | ✅ | ✅ | 对齐 |
| 共享 baidu_std channel | N/A | ✅ | ✅ | 对齐 |
