# Us3TurboAccess 代码 Review 流程

> 本流程适用于 `Us3TurboAccess/` 全仓 review，覆盖 client / gateway / examples /
> scripts 任意子模块。目标是把每一轮 review 跑成可重复、有产出物的工序，而不是
> 凭手感读代码。
>
> 三条强制原则（任何 review 都要兜底）：
>
> 1. **安全 / 漏洞 / 清晰度 / 简洁性**：四个维度逐文件过一遍，能优化的就在 review
>    时直接动手而不是堆 TODO。
> 2. **可读性优先，不允许 lambda 表达式**：所有循环回调 / executor 任务 / RAII
>    辅助都用命名函数 / 成员函数指针 / functor，避免 `[&] / [=] / []` 在调用现场
>    捕获状态。
> 3. **接口注释分层**：`include/**/*.h` 等对外头文件用规范英文 Doxygen；`src/**/*.{h,cpp}`
>    内部实现关键流程用简洁中文，**解释 why 不解释 what**。

| 字段 | 值 |
|------|----|
| 适用范围 | `client/` `gateway/` `include/` `examples/` `scripts/` |
| 不适用 | 第三方 `FusionAccess/third_party/*`、`build/*` 编译产物 |
| 一次 review 粒度 | 一个子模块（≤ ~3000 行）或一次 commit-set 内全部改动 |
| 一次 review 周期 | 检查 + 优化 + 验证 + 记录 一起做完，不留尾巴 |

---

## 一、Review 三阶段

```
        ┌───────────── Pre  ──────────────┐
        │ scope 圈定 / checklist 落地     │
        │ 基线 build & test 通过          │
        └───────────────┬─────────────────┘
                        ▼
        ┌───────────── Pass 1 (检查)  ────┐
        │ 安全 ─ 漏洞 ─ 清晰度 ─ 简洁性    │
        │ 发现一条记一条，先不动代码       │
        └───────────────┬─────────────────┘
                        ▼
        ┌───────────── Pass 2 (优化)  ────┐
        │ 按 finding 逐项 in-place 修复    │
        │ 每个 fix 跑一次目标 verify       │
        └───────────────┬─────────────────┘
                        ▼
        ┌───────────── Post ─────────────┐
        │ regression: rdma_test / http_test│
        │            / bench / verify     │
        │ 写 review 记录 docs/review-*.md │
        │ 提交 commit (一次 review 一 PR)  │
        └─────────────────────────────────┘
```

### 1. Pre：圈定 scope + 基线

- **明确范围**：单子模块 / 单 commit-set / 单 PR diff。把要 review 的文件路径
  列出来，写到 `docs/review-YYYY-MM-DD-<topic>.md` 的开头。
- **基线必须先绿**：跑一遍当前的 verify / regression 脚本，确认基线干净，
  避免 review 完了不知道是 review 引入的回归还是原本就坏的。
  ```
  ./do_make.sh
  bash scripts/rdma_test.sh
  bash scripts/http_test.sh
  bash scripts/bench_compare.sh --rounds 1   # 速度优先，只看不退化
  ```
- **生成 review checklist**：复制本文档 § 三 的清单到 review 记录里，准备逐项
  打勾。

### 2. Pass 1：检查（不改代码）

按 § 三 的四个维度逐文件过。每发现一个问题写一行：

```
[ 文件:行号 ] [ 维度 ] [ 严重度 high/med/low ] 一句话描述
```

**这一阶段不要边读边改**。一是容易跑题，二是改之前要先看完整模块才能判断
"局部改"会不会破坏全局假设。

发现一旦超过 ~15 条就先停下来，进入 Pass 2。

### 3. Pass 2：优化（动代码）

按 finding 列表逐条 fix。每条 fix 必须满足：

- **小步走**：一个 finding 一次 Edit，不混合多条改动。
- **fix 后立即验证**：能跑就跑对应的 verify example / unit test；不能跑就 build
  一遍至少保证编译过。
- **记录**：在 finding 旁边标 `✓ fixed at <commit-sha-prefix or "WIP">` 或
  `⊘ deferred: <reason>`。

不一定每条都 fix；可以推迟，但**必须给推迟理由**（"风险太大要单独评审" /
"需要 proto 变更" / "和别人 in-flight 工作冲突"等），不允许"先放一下"这种
模糊推迟。

### 4. Post：回归 + 记录 + 提交

- **回归**：重跑 Pre 阶段的全部 verify 脚本，**必须全过**。任何脚本不过就回退
  最后一条 fix 重 review。
- **bench 对比**（如果改动可能影响性能）：跑 `scripts/sustained_bench.sh
  --rounds 3` 对照前后 throughput / CPU·MiB，挂在 review 记录里。
- **review 记录归档**：把 `docs/review-YYYY-MM-DD-<topic>.md` 写完，包含：
  - scope 列表
  - finding 表（含严重度 + fix 状态）
  - 验证结果
  - 推迟项的具体延后到何时 / 何条件
- **commit**：一次 review 一个 commit，commit message 引用 review 记录文件名。

---

## 二、对外接口注释规范

### 2.1 范围

| 路径 | 角色 | 注释语言 | 注释风格 |
|------|------|---------|---------|
| `client/include/us3_turbo_access/client/*.h` | SDK 公开 API | **English** | Doxygen `/** ... */` |
| `gateway/include/us3_turbo_access/gateway/*.h` | Gateway 嵌入接口 | **English** | Doxygen |
| `include/us3_turbo_access/common/*.h` | 跨进程 wire 定义 | **English** | Doxygen |
| `client/src/**/*.{h,cpp}` | 内部实现 | **中文** | `//` 行注释或 `/* */` 短块 |
| `gateway/src/**/*.{h,cpp}` | 内部实现 | **中文** | `//` 行注释或 `/* */` 短块 |
| `examples/*.cpp` | 示例 | 中英文都行 | 顶部一段总说明 |
| `scripts/*.sh` | 运维 | 中文 | `#` 行注释 |

### 2.2 公开 API（英文 Doxygen）

每一个公开 class / function / 公开 struct 字段都需要文档。模板：

```cpp
/**
 * @brief One-line summary in imperative present tense.
 *
 * Optional paragraph(s) explaining intent, invariants, threading model,
 * resource ownership. Avoid restating the signature.
 *
 * @param ptr  device pointer obtained from cudaMalloc. Must be non-null.
 * @param size byte size of the region; rounded up to the next 4 KiB by
 *             the underlying driver.
 * @return     Success carries no payload (Result<bool>::Success(true));
 *             failure carries a retryable Error when the BAR1 pin
 *             succeeded mid-call and was rolled back.
 *
 * @warning Must be called BEFORE cudaFree(ptr). Skipping the unregister
 *          step can deadlock the nvidia-fs kernel module per NVIDIA's
 *          official guidance.
 * @note    Idempotent: re-registering the same ptr is a no-op success.
 *
 * @see Client::UnregisterDeviceBuffer
 */
[[nodiscard]] Result<bool> RegisterDeviceBuffer(void* ptr, std::size_t size);
```

规则：
- 用祈使句现在时（"Register a GPU buffer ..."，不是 "Registers / Will register"）。
- `@brief` 一行，句号结尾。
- 至少有 `@param` + `@return` + 一个语义补充段（@warning / @note / @see / @pre /
  @post）；如果没有可补充的 → 这个 API 是不是太简单不该公开？
- 不写废话型注释（不要 `@param ptr the ptr`、`@return result of operation`）。
- 不引用内部实现文件路径（接口稳定，路径会变）。

### 2.3 内部实现（简洁中文）

**核心原则**：注释解释 *why*，不解释 *what*。代码本身已经讲清楚 what，注释要补
能从代码读不出的上下文。

合格的内部注释长这样：

```cpp
/* 描述符表是 process-wide：单例 cuObjClient + descriptor 缓存。
 * 注册路径（RegisterBuffer / UnregisterBuffer）短锁串行；
 * 数据面 AcquireToken 无锁，命中缓存后只调 GetRDMAToken。
 * 这条约束来自 PoC：cuObjPut 并发不安全但 GetRDMAToken 安全。 */
class CuObjState { ... };
```

不合格的内部注释（删掉）：

```cpp
// 调用 RegisterUnderLock                  ← 重复代码本身
return RegisterUnderLock(st, ptr, size);

// for 循环遍历所有 part                  ← 废话
for (auto& part : parts) { ... }

// TODO: 后面再优化                       ← 没说什么优化、什么条件下做
```

风格细节：
- 单行用 `//`；多行用 `/* */`，**禁止**多行 `//` 堆砌。
- 中文标点 + 西文标点混用按上下文：纯中文句用中文句号；含代码/标识符的句子
  用英文标点。**整段保持一致**。
- 长度上限：单条注释 ≤ 4 行。再长就拆成另一段说明放在函数前面。
- 函数前 doc-block 用 `/** */` 也可以（即使是内部函数），但内容仍是中文。

### 2.4 注释 review 清单

- [ ] 公开头文件每个 declaration 都有 Doxygen 注释（英文）
- [ ] 内部实现的"非显然"决策都有 why-注释（中文）
- [ ] 没有 `// TODO` / `// FIXME` 无主项（要么 fix 要么删要么开 issue 引用）
- [ ] 没有"复述代码"的废话注释
- [ ] 中英文不混杂（接口英文、实现中文，不互相侵入）

---

## 三、Review 四维度 checklist

### 3.1 安全（high 严重度）

**SDK 层**：
- [ ] 公开 API 输入校验：null ptr / 0 size / overflow 计算 / 越界字符串 length
- [ ] 内存安全：`new` / `cudaMalloc` / `ibv_reg_mr` / dlopen 都有对应 RAII 守卫
- [ ] 线程安全：共享 state 是否被 mutex 保护；mutex 边界是否最小化（不持锁调
      RPC / syscall）
- [ ] 整数溢出：`size_t` 加减乘除前是否有上限检查；`offset + length` 不溢出
- [ ] 资源泄漏：异常 / 错误路径上的 cudaFree / ibv_dereg_mr / dlclose

**Gateway 层**：
- [ ] 输入来自 wire 的字段全部校验（bucket / key / size / etag 长度上限）
- [ ] auth / idempotency / replay：每个变更操作有 idempotency_key 校验
- [ ] 拒绝服务：max in-flight session / max upload parts / max total bytes
- [ ] 内存 backend：capacity 检查；driver 拒掉的请求要返 retryable 错误
- [ ] RDMA / GDS：QP / MR 池要有上限，避免 BAR1 / iWARP card 资源被外部耗光

**通用**：
- [ ] 任何 `unsafe_cast` / `reinterpret_cast` / C-style cast 有充足理由
- [ ] log 不打 PII / 不打 raw bucket-key（如果是敏感的）
- [ ] 临时文件 / pipe / shared memory 权限 0600

### 3.2 漏洞模式（high / medium）

- [ ] **TOCTOU**：check + use 之间没有可能被改写的中间窗口
- [ ] **Double free / use-after-free**：智能指针 vs 裸指针所有权清晰
- [ ] **Race condition**：双 check locking、读修改写无原子保护、`std::shared_ptr`
      的别名拷贝竞争
- [ ] **Deadlock**：锁获取顺序在所有路径一致；不持锁调外部函数（RPC、syscall）
- [ ] **Integer overflow → buffer overflow**：尤其在 wire-format 解析时
- [ ] **未初始化变量**：尤其在 `union` / aggregate-init / `alignas(...) std::byte[]`
      场景
- [ ] **错误码丢失**：每个 `Result<T>` 返回必须 .success() 检查或显式 `(void)`
      并加注释解释

### 3.3 清晰度（medium）

- [ ] **单一职责**：函数 ≤ 80 行，超出就拆；class 公开 method ≤ 12 个
- [ ] **命名**：
  - 类型 `UpperCamelCase`
  - 函数 / 公开成员 `UpperCamelCase`
  - 变量 / 私有成员 `lower_snake_case`，私有成员加尾下划线 `name_`
  - 常量 / enum 值 `kPascalCase`
  - 模板形参 `T` / `BufferPointer` / `Op` 等有意义的 PascalCase
- [ ] **不要简写**：`cfg` `req` `mgr` `mu` 在长函数里展开成 `config` `request`
      `manager` `mutex`（`mu` 在局部 ≤ 10 行作用域可以保留）
- [ ] **没有魔法数字**：超过 1 处使用的整型 / 时间值 / 容量 → 提到 `constexpr`
      常量或 ClientOptions / 配置项
- [ ] **依赖方向**：分层从下到上，禁止 `core/` include `transports/`、禁止
      `transports/` include `core/client/`（仅 `core/<path>` 可以下沉）
- [ ] **接口 vs 实现分离**：能在 `.cpp` 里的不要放 `.h`
- [ ] **错误消息**：人类可读 + 含足够定位上下文（哪个 op、哪个 request_id、
      retryable 否）

### 3.4 简洁性（low / medium）

- [ ] **死代码** / 未使用的 include / 未用的私有成员 → 删
- [ ] **重复逻辑**：≥ 3 处相同 5 行以上代码 → 抽函数
- [ ] **过度抽象**：单一调用点的 helper class / template 参数 / virtual → 内联
- [ ] **过早优化**：未经 bench 验证就上的 cache / pool / SIMD → 回退到直白实现
- [ ] **构造开销**：hot path 上避免 string + / vector resize / map 临时构造
- [ ] **没用上的 lazy 初始化**：start-up 期能 eager 算的就 eager
- [ ] **API surface area**：能 private 的不要 protected；能 unexposed 的不要
      `.h` exposure

---

## 四、Lambda 表达式禁令的细节

### 4.1 为什么不用 lambda

- **栈追踪可读性**：`operator()` + 无名 closure 在 gdb / log 里只显示
  `<lambda(123:45)::operator()>`，命名函数显示具体函数名
- **逃逸生命周期**：`[&]` 捕获在异步 / 跨线程 / future / executor 场景容易出
  use-after-scope；命名函数 + 显式参数传递不会
- **可测试性**：lambda 不能单独测；命名函数能拎出来 unit test
- **可重用性**：lambda 是单一调用点；命名函数允许第二个调用方复用
- **review 友好**：lambda 在调用现场内联很多行代码 → 函数变长 → 难审

### 4.2 替代方案

**循环回调 / executor 任务 → 命名 functor 或成员函数指针**

```cpp
// ❌ 不允许
executor.Submit([core = core_.get(), request, buffer]() {
  return core->transfer_router().PutObject(request, buffer);
});

// ✓ 推荐：私有成员函数 + std::bind / 显式参数对象
struct PutObjectJob {
  ClientCore* core;
  RequestOptions request;
  ConstBufferView buffer;
  Result<TransferOutcome> operator()() const {
    return core->transfer_router().PutObject(request, buffer);
  }
};
executor.Submit(PutObjectJob{core_.get(), request, buffer});
```

**RAII / scope guard → 命名 class**

```cpp
// ❌
auto guard = MakeScopeExit([&]() { cudaFree(ptr); });

// ✓
class CudaFreeGuard {
 public:
  explicit CudaFreeGuard(void* p) : p_(p) {}
  ~CudaFreeGuard() { if (p_) cudaFree(p_); }
  CudaFreeGuard(const CudaFreeGuard&) = delete;
  CudaFreeGuard& operator=(const CudaFreeGuard&) = delete;
 private:
  void* p_;
};
CudaFreeGuard guard(ptr);
```

**算法回调（std::sort / std::find_if） → 函数指针或 functor struct**

```cpp
// ❌
std::sort(v.begin(), v.end(), [](auto& a, auto& b) {
  return a.part_number < b.part_number;
});

// ✓
struct PartNumberLess {
  bool operator()(const PartCompletion& a, const PartCompletion& b) const {
    return a.part_number < b.part_number;
  }
};
std::sort(v.begin(), v.end(), PartNumberLess{});
```

**worker 线程函数 → 命名静态函数**

```cpp
// ❌
std::thread t([&]() { while (!stop) DoWork(); });

// ✓
class WorkerLoop {
 public:
  WorkerLoop(SharedState* s) : state_(s) {}
  void operator()() {
    while (!state_->stop.load()) DoWork(state_);
  }
 private:
  SharedState* state_;
};
std::thread t(WorkerLoop{&state});
```

### 4.3 容忍的例外

只在以下三种情况下允许 lambda（review 时仍要 challenge 一遍）：

1. **STL 单语句谓词**，且仅在文件内单点使用，且 ≤ 1 行：
   ```cpp
   bool any_failed = std::any_of(results.begin(), results.end(),
                                  [](const auto& r) { return !r.success(); });
   ```
2. **brpc / 第三方库强制要求** `std::function<void()>` 而对方接口无法绕开
3. **测试代码**（`tests/` / google-test 内）

任何一处 lambda 必须在代码旁注明属于上述哪一类，否则 review 拒。

### 4.4 现有代码的迁移策略

当前仓库（`grep -rn "\[&\]\|\[=\]" --include="*.cpp"` 计数 ~24 处）有遗留 lambda。
**不要为了 zero-lambda 一次性全改**，按以下规则渐进：

- review 到的文件，**顺手清理该文件内全部 lambda**
- 新增代码**零 lambda**
- 跨文件批量改造**独立成单独 commit**，不混在功能 review 里

---

## 五、Review 记录模板

新建 `docs/review-YYYY-MM-DD-<topic>.md`：

```markdown
# Review: <topic>

| 字段 | 值 |
|------|----|
| Reviewer | <name> |
| 日期 | YYYY-MM-DD |
| Scope | <files / commit-range> |
| 基线 build | <commit-sha> ✓ green |
| 基线 verify | rdma_test ✓ / http_test ✓ |

## Findings

| # | 文件:行 | 维度 | 严重度 | 描述 | 状态 |
|---|--------|------|--------|------|------|
| 1 | client/src/foo.cpp:42 | 安全 | high | 未校验 size overflow | ✓ fixed |
| 2 | client/src/bar.h:18 | 清晰度 | low | 函数名 doWork 太泛 | ⊘ defer: 跨模块改名独立 PR |
| ... | | | | | |

## 优化记录

- finding #1: 加 `size > 0 && size <= kMaxSize` 校验，单测覆盖 size=0 / size=SIZE_MAX。
- finding #3: 抽 `ExtractEtag` 函数取代 3 处重复 string 解析。

## 验证

- [x] build green: `./do_make.sh`
- [x] regression: `bash scripts/rdma_test.sh`
- [x] regression: `bash scripts/http_test.sh`
- [x] bench 不退化: rdma put 1020 → 1018 MiB/s, gds put 6079 → 6088 MiB/s
- [x] verify example: `gds_verify_example` exit 0

## 推迟项

| # | 描述 | 推迟原因 | 触发条件 |
|---|------|---------|---------|
| 2 | 改名 doWork | 跨模块影响 8 个 caller | 下个 sprint 单独 PR |

## Commit

`<commit-sha>` "<commit subject>"
```

---

## 六、Reviewer 自检清单（提交前最后 5 分钟）

- [ ] 所有 finding 都有 ✓ fixed 或 ⊘ deferred + 理由
- [ ] 公开 API 注释 100% 英文 Doxygen
- [ ] 新增 / 修改的关键内部流程有中文 why-注释
- [ ] 零 lambda（或新增 lambda 全部标注例外类别）
- [ ] 无 `// TODO` / `// FIXME` 无主项
- [ ] 无注释掉的死代码
- [ ] regression 脚本全过
- [ ] commit message 引用 `docs/review-*.md`
- [ ] 没有未跟踪的 build 产物 / 临时文件被一并 commit
- [ ] 没有 `git config` 改动 / 没有跳过 hook

---

## 七、常见反模式速查

| 反模式 | 修正方向 |
|--------|---------|
| `[&] (...) { ... }` 长 lambda | 抽命名函数 / functor |
| `if (rc) return Failure();` 不 log | 加错误上下文 + retryable 标记 |
| `auto x = ...;` 类型不显然 | 写出类型 |
| `using namespace std;` 在 header | 删 |
| `std::shared_ptr` 在内部容器内 | 评估能否 `unique_ptr` + raw observer |
| `mutex` 守一切 | 拆短锁；用 atomic 替代纯计数 |
| 函数 > 100 行 | 拆 |
| 头文件 include 实现文件 | 反层依赖，重组 |
| `if (x == true)` / `if (x == false)` | `if (x)` / `if (!x)` |
| 双否定 `!(a != b)` | `a == b` |
| 魔法数字 `4096`、`128 * 1024 * 1024` | constexpr 常量 |
| `static` 大对象在 header | 改 inline 或 .cpp |
| 异常被 catch (...) 吞 | 至少记录 type + what()，决定 retryable |
| 错误消息丢失 request_id | 错误工厂统一注入 |
| `*` 接收 raw pointer 当 borrow | 用引用 `&` 表达 borrow，`*` 仅表 nullable |

---

## 八、与现有文档的关系

- `docs/gateway.md` — gateway 架构 / 协议，review 时作为"是否符合架构"的参照
- `docs/gateway-rewrite-plan.md` — 历史重写计划，已 done，仅作背景
- `docs/gds-rewrite.md` — GDS 通路重写的设计 + 性能验证，是"已 review 通过"
  的范例

本流程文档**不替代上面三份**，是 review 工序规范；做 GDS 相关 review 时仍要
读 `gds-rewrite.md` 了解设计意图。
