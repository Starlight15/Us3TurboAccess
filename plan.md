## Context
当前 `Us3TurboAccess` 的主要问题已经从“去重”变成“职责边界不清晰”：
- `Client::Impl` 同时承担了 runtime 初始化、control-plane object API、transfer API、path 组装、path 选择，入口过厚。
- 对外 API 只有一个 `Client` 门面，`Initialize/Shutdown/Head/Get/Put` 都混在一起，接口分层不清楚。
- GDS 的 `Get/Put` 虽然已经与 RDMA 隔离，但仍然在同一个 executor 文件里同时承载两条操作流程。
- 后续还要补 native RDMA，因此现在需要先把“API 层 / 初始化装配层 / 通路调度层 / 通路内操作流”拆干净，避免 RDMA 落地时继续把职责堆回 `Client`。

这轮目标是：
**保留顶层统一抽象与配置选路，同时把对外 API、初始化装配、对象控制面、对象传输面、以及 GDS/RDMA 各自的 put/get 流程拆成清晰模块。**

## Recommended approach
1. 对外 API 从单一 `Client` 拆成“根 API + 子 API”。
   - 保留 `Client` 作为顶层入口，但它不再直接暴露所有实现细节。
   - 建议改成：
     - `Client`：生命周期与 capability 查询，只负责 `Initialize/Shutdown/initialized/capabilities`
     - `ObjectApi`：对外提供 `HeadObject`
     - `TransferApi`：对外提供 `GetObject/PutObject`
   - `Client` 持有并暴露这两个子 API，例如：
     - `client.Objects().HeadObject(...)`
     - `client.Transfers().GetObject(...) / PutObject(...)`
   - 这样 init 和 object/transfer 行为在 API 层就分开了。

2. 新增单独的 runtime/bootstrap 装配层，移出 `Client::Impl` 的对象图构造职责。
   - 当前 `client/src/core/client.cpp` 里直接 new/持有：
     - `ControlPlaneClient`
     - `GdsRpcClient`
     - `ObjectRequestBuilder`
     - `SessionNegotiator`
     - `GdsMemoryRegistry`
     - `CuObjectClient`
     - `RdmaTransport`
     - `GdsPathExecutor`
     - `RdmaPathExecutor`
     - `TransferOrchestrator`
   - 建议新增一层类似 `ClientRuntime` / `ClientRuntimeBuilder` 的内部模块，专门负责：
     - 依赖装配
     - Initialize / Shutdown
     - 保存 runtime state
   - `Client` 只依赖 runtime 和 API 对象，不再自己承担整棵依赖树的组装。

3. object control-plane API 单独成层，不再从 `Client::Impl` 直连 `ControlPlaneClient`。
   - 新增明确边界的对象控制面模块，例如：
     - `client/src/api/object_api.h/.cpp`
   - 该层内部复用：
     - `ObjectRequestBuilder::BuildRpcRequestContext`
     - `ControlPlaneClient::HeadObject`
   - 只承载对象级控制面语义（现在是 Head，后续 Delete/Stat 也可收进来）。

4. transfer API 与 path 调度单独成层。
   - 新增：
     - `client/src/api/transfer_api.h/.cpp`
   - 该层只负责：
     - 参数入口
     - initialized 检查
     - 调用 `TransferOrchestrator`
   - 保留 `TransferOrchestrator` 作为 path-independent 调度层，不把 GDS/RDMA 逻辑上提到 API 层。

5. GDS 与 RDMA 保持隔离，但每条通路内部再按 operation 拆文件。
   - GDS 建议从当前：
     - `client/src/core/gds/gds_path_executor.h/.cpp`
     拆成：
     - `client/src/core/gds/gds_path_executor.h/.cpp`：仅保留 executor 壳、available、公共依赖持有
     - `client/src/core/gds/gds_get_flow.cpp`
     - `client/src/core/gds/gds_put_flow.cpp`
   - 其中每个 flow 文件各自实现：
     - register buffer
     - negotiate session
     - execute cuObject transfer
   - RDMA 也按同样结构预留：
     - `client/src/core/rdma/rdma_get_flow.cpp`
     - `client/src/core/rdma/rdma_put_flow.cpp`
   - 即便当前 RDMA 仍是占位实现，也先把文件边界定好，后续实现直接落到各自 flow 文件里。

6. path executor 的职责收紧为“通路入口”，不再内嵌完整流程细节。
   - `GdsPathExecutor::GetObject/PutObject` 改为转调各自 flow。
   - `RdmaPathExecutor::GetObject/PutObject` 也改为转调各自 flow。
   - 这样顶层统一接口仍然是 `PathExecutor`，但每条通路的具体操作流程已经下沉到独立文件。

7. 保持现有可复用基础组件，不重新设计伪统一传输层。
   - 继续复用：
     - `SessionNegotiator`
     - `ObjectRequestBuilder`
     - `ControlPlaneClient`
     - `GdsRpcClient`
     - `GdsMemoryRegistry`
     - `CuObjectClient`
   - 不新增跨 GDS/RDMA 的“通用 transfer flow 基类”，避免再次引入假抽象。

## Critical files
- `Us3TurboAccess/client/include/us3_turbo_access/client/client.h`
- `Us3TurboAccess/client/src/core/client.cpp`
- `Us3TurboAccess/client/src/core/transfer_orchestrator.h`
- `Us3TurboAccess/client/src/core/transfer_orchestrator.cpp`
- `Us3TurboAccess/client/src/core/path_executor.h`
- `Us3TurboAccess/client/src/core/gds/gds_path_executor.h`
- `Us3TurboAccess/client/src/core/gds/gds_path_executor.cpp`
- `Us3TurboAccess/client/src/core/rdma/rdma_path_executor.h`
- `Us3TurboAccess/client/src/transports/rdma/rdma_transport.h`
- `Us3TurboAccess/client/src/core/session_negotiator.h`
- `Us3TurboAccess/client/src/core/session_negotiator.cpp`
- `Us3TurboAccess/client/src/core/object_request_builder.h`
- `Us3TurboAccess/client/src/core/object_request_builder.cpp`
- `Us3TurboAccess/client/src/control/control_plane_client.h`
- `Us3TurboAccess/client/src/control/control_plane_client.cpp`
- `Us3TurboAccess/client/src/http/gds_rpc_client.h`
- `Us3TurboAccess/client/src/http/gds_rpc_client.cpp`
- `Us3TurboAccess/client/src/transports/gds/cuobject_client.h`
- `Us3TurboAccess/client/src/transports/gds/cuobject_client.cpp`
- `Us3TurboAccess/client/src/transports/gds/gds_memory_registry.h`
- `Us3TurboAccess/client/src/transports/gds/gds_memory_registry.cpp`
- `Us3TurboAccess/client/CMakeLists.txt`

## Existing code to reuse
- API 当前入口：
  - `Us3TurboAccess/client/include/us3_turbo_access/client/client.h` `Client`
- 顶层统一调度：
  - `Us3TurboAccess/client/src/core/transfer_orchestrator.cpp` `TransferOrchestrator::GetObject/PutObject`
  - `Us3TurboAccess/client/src/core/path_executor.h` `PathExecutor`
- GDS 通路公共能力：
  - `Us3TurboAccess/client/src/core/session_negotiator.cpp` `SessionNegotiator::NegotiateSession`
  - `Us3TurboAccess/client/src/transports/gds/gds_memory_registry.cpp` `GdsMemoryRegistry::Register`
  - `Us3TurboAccess/client/src/transports/gds/cuobject_client.cpp` `CuObjectClient::ExecuteGet/ExecutePut`
  - `Us3TurboAccess/client/src/core/object_request_builder.cpp` `BuildGdsChunkRequest`
- object control-plane 能力：
  - `Us3TurboAccess/client/src/control/control_plane_client.cpp` `ControlPlaneClient::HeadObject`
  - `Us3TurboAccess/client/src/core/object_request_builder.cpp` `BuildRpcRequestContext`
- RDMA 顶层隔离入口：
  - `Us3TurboAccess/client/src/core/rdma/rdma_path_executor.h` `RdmaPathExecutor`
  - `Us3TurboAccess/client/src/transports/rdma/rdma_transport.h` `RdmaTransport`

## Implementation steps
1. 重构 public API：在 `client/include` 下引入 `ObjectApi` / `TransferApi`，调整 `Client` 为根入口与生命周期持有者。
2. 新增内部 runtime 模块，承接当前 `Client::Impl` 的依赖装配、Initialize、Shutdown、runtime state。
3. 新增 `api/object_api.*`，把 `HeadObject` 从 `Client::Impl` 中抽出。
4. 新增 `api/transfer_api.*`，把 `GetObject/PutObject` 从 `Client::Impl` 中抽出，并只对接 `TransferOrchestrator`。
5. 将 GDS 的 `Get/Put` 流程拆到独立 flow 文件，`GdsPathExecutor` 仅保留通路入口职责。
6. 为 RDMA 建立对称的 `get/put flow` 文件边界，即便当前仍返回 unsupported，也先固定结构。
7. 更新 `CMakeLists.txt`，确保新 API/runtime/flow 文件全部纳入构建。
8. 更新 example 调用方式，使其走新的 API 入口而非旧的混合式 `Client` 方法。

## Verification
1. 构建 `us3_turbo_access_client`，确认 public API 改造后头文件与链接通过。
2. 确认 `Client` 只负责：
   - lifecycle
   - capability 查询
   - 暴露子 API 入口
3. 确认 `ObjectApi` 只处理对象控制面接口，`TransferApi` 只处理数据传输接口。
4. 确认 `TransferOrchestrator` 仍然只做 path 选择，不承载 GDS/RDMA 具体流程。
5. 确认 GDS `Get` 与 `Put` 已分别落在独立代码文件中。
6. 确认 RDMA 也已建立独立 `Get/Put` flow 文件边界，并保持与 GDS 隔离。
7. 重新构建并运行 `us3_turbo_access_gds_example`，验证 PUT / HEAD / GET 闭环仍然成功。
