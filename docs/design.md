# 设计说明

## 目标

本提交物通过 LLVM Pass 在 IR 层分析官方 baseline 中的算子调用和循环结构，将可并行的 `trsm`、`madd` 调用改写为运行时异步任务提交，并在对应 loop exit 插入同步点。官方 `cholesky`、`trsm`、`madd` 算子实现不被重定义、替换或绕过。

当前实现不再使用“直接清空 `block_cholesky` 函数体并替换成 runtime 入口”的方案。那个方案虽然能工作，但过于接近函数级替换，不符合本赛题强调的“LLVM Pass 分析算子依赖关系并插入运行时接口”的方向。

## Pass 设计

Pass 名称：

```text
contestant-pass
```

Pass 插件：

```text
pass/libcontestant_pass.so
```

Pass 类型：

```text
LLVM New Pass Manager FunctionPass
```

Pass 使用 `LoopAnalysis` 获取 `LoopInfo`，只处理官方 `contest::block_cholesky`：

- 函数名为官方 C++ mangled name `_ZN7contest14block_choleskyEPKdPdii`，或包含 `block_cholesky`。
- 返回类型为 `i32`。
- 参数数量为 4。

### IR 版本化

为了避免小 block 场景下任务提交开销超过计算收益，Pass 采用 IR 版本化：

```text
compiler2026_async_impl(A, L, n, b)
```

Pass 从官方 `block_cholesky` 克隆出 async 版本，只在 async clone 中改写 `trsm/madd` call site；原始 `block_cholesky` 函数体保留为串行路径。入口分支调用 runtime predicate：

```text
if (compiler2026_runtime_should_async(n, b))
    return compiler2026_async_impl(A, L, n, b);
else
    run original serial IR path;
```

默认 predicate 仍使用 `b >= 18`、block 数大于等于 2 且可用线程数大于 1；`COMPILER2026_ASYNC_MIN_B` 和 `COMPILER2026_ASYNC_MIN_BLOCKS` 可以覆盖阈值，用于 smoke/profile/benchmark 调参。

async clone 是从官方 baseline IR 克隆出来的，不是手写算法替换。

### 算子调用转换

在 async path 中：

- `cholesky(...)` 保持同步原始调用。
- `trsm(...)` 被 outline 到 Pass 生成的 IR task function：

```llvm
define internal void @compiler2026_task_trsm(ptr %ctx) {
  ...
  call void @trsm(ptr %A, ptr %L, ptr %X, i32 %b, i32 %lda)
  ret void
}
```

- `madd(...)` 被 outline 到 Pass 生成的 IR task function：

```llvm
define internal void @compiler2026_task_madd(ptr %ctx) {
  ...
  call void @madd(ptr %A, ptr %B, ptr %C, i32 %b, i32 %lda)
  ret void
}
```

- 原始 call site 分配参数上下文并提交 task function：

```llvm
%ctx = call ptr @compiler2026_runtime_alloc(...)
...
call void @compiler2026_runtime_submit(ptr @compiler2026_task_trsm, ptr %ctx)
```

Pass 根据 `LoopInfo` 插入同步：

- `trsm` 所在 loop 的 exit block 插入 `compiler2026_runtime_wait()`。
- `madd` 所在内层 loop 的父 loop exit block 插入 `compiler2026_runtime_wait()`，使同一 panel 的 `madd` 任务完成后再进入下一 panel。
- 函数 async path 入口插入 `compiler2026_runtime_begin(n, b)`。
- 函数 async path 入口注册 `compiler2026_task_trsm/madd` 的 profile 名称；该注册只提供观测标签，不改变调度语义。
- async path 返回前插入 `compiler2026_runtime_end()`。

优化后的 IR 仍保留原始 `block_cholesky` 的串行循环结构，并额外生成 async clone。检查命令：

```bash
llvm-dis build/optimization_benchmarks/ir/app.opt.bc -o - \
  | grep -n "compiler2026_runtime_should_async\\|compiler2026_async_impl\\|compiler2026_task_\\|compiler2026_runtime_submit\\|compiler2026_runtime_wait\\|call.*@trsm\\|call.*@madd\\|define.*block_cholesky"
```

已验证 IR 片段包含：

```text
define dso_local noundef i32 @_ZN7contest14block_choleskyEPKdPdii(...)
  call i32 @compiler2026_runtime_should_async(...)
  call i32 @compiler2026_async_impl(...)

define internal noundef i32 @compiler2026_async_impl(...)
  call void @compiler2026_runtime_submit(ptr @compiler2026_task_trsm, ptr ...)
  call void @compiler2026_runtime_submit(ptr @compiler2026_task_madd, ptr ...)

define internal void @compiler2026_task_trsm(ptr ...)
  call void @trsm(...)

define internal void @compiler2026_task_madd(ptr ...)
  call void @madd(...)
```

## Runtime 设计

Runtime 静态库：

```text
runtime/libcontestant_runtime.a
```

Runtime API：

```c
extern "C" void compiler2026_runtime_begin(int n, int b);
extern "C" int compiler2026_runtime_should_async(int n, int b);
extern "C" void compiler2026_runtime_register_task(void (*fn)(void *), const char *name);
extern "C" void *compiler2026_runtime_alloc(std::size_t size);
extern "C" void compiler2026_runtime_submit(void (*fn)(void *), void *ctx);
extern "C" void compiler2026_runtime_submit_deps(
    void (*fn)(void *), void *ctx, int dep_a, int dep_b, int output);
extern "C" void compiler2026_runtime_wait();
extern "C" void compiler2026_runtime_end();
```

Runtime 内部维护一个 thread-local `AsyncRuntime`：

- `runtime_should_async` 集中管理 async path 入口阈值和线程数判断，使 Pass 入口分支、runtime 线程选择和 benchmark 记录的 `async_min_b` / `async_min_blocks` 保持一致。
- `runtime_alloc` 为 Pass 生成的 task context 分配内存。
- `runtime_submit` 只接收 task function 指针和 context 指针。
- `runtime_submit_deps` 额外接收两个输入 block key 和一个输出 block key；runtime 用这些 key 维护 latest-producer 依赖和 ready queue，但不理解具体算子语义。
- `runtime_submit_deps3` 是三输入依赖版本，用于实验性的跨 panel DAG：`madd` 需要同时依赖两个 `trsm` 输入和自身输出块的 previous producer。
- 当前 panel-local DAG 下，Pass 对 `trsm` submit 保留 output key，对 `madd` submit 使用 output key `-1`。原因是 panel 末尾仍有 wait，`madd` 输出在当前 DAG 作用域内没有后续 async consumer；这样可以减少 runtime 对无消费者 `madd` 节点的 `latest_producer_` 哈希表更新。后续做跨 panel DAG 时，需要重新把相关 `madd` 输出纳入依赖图。
- worker 线程从队列中取任务，调用 Pass 生成的 task function。
- `wait` 等待队列为空且所有运行中任务完成；主线程也会参与执行队列任务。
- 当前 DAG 作用域是 panel-local；`wait` 确认 DAG 节点全部完成后会清理已完成节点和 latest-producer 表，profile 计数保留到 `end` 汇报。
- `end` 做最终等待并重置本轮 arena，不销毁可复用 worker 池。
- ready queue、DAG node vector、latest-producer 表和批量提交状态的 reset 在 runtime mutex 下执行，避免 worker 池复用时清理调度状态和 worker 观察队列状态并发。
- worker 池按当前问题规模和 `COMPILER2026_DAG_THREADS` 选择线程数；线程数变化时才重建。
- task context 使用 per-call arena 分配，避免每个 task 单独 `malloc/free`。
- runtime 按首个 panel 的 `trsm + madd` 任务数预估 panel-local task 容量，并复用 ready queue、DAG node vector 和 latest-producer hash table 的容量。
- DAG successor 边使用 runtime 统一的连续 edge pool；每个 producer node 只保存 successor 链表的 head/tail 和计数，避免为高 fanout `trsm` producer 维护大量独立小 vector。
- runtime 根据 `b`、block 数和参与线程数选择小批量提交和批量出队策略：`b <= 64` 默认批量上限为 `8`，当 panel block 数相对线程数偏少时自动收窄批量，避免少量 ready task 被一次取走过多；`b > 128` 保持单任务粒度。
- `COMPILER2026_TASK_BATCH` 可覆盖默认批量大小，用于真实多核平台调参。
- `COMPILER2026_DAG_PROFILE=1` 打开轻量 profiling，向 stderr 输出 async path 判定次数和原因、任务数、队列等待时间、执行时间、worker idle 时间、`wait()` 调用次数和总耗时、`wait()` 入口 ready/active/DAG live pressure、主线程在 `wait()` 中无 ready task 可执行的等待时间、批量出队信息、ready queue 宽度采样、DAG 节点/边/已满足依赖/缺失依赖/释放批量/fanout/live 统计，以及按已注册 task 名称聚合的 `trsm/madd` 统计。
- smoke 和 benchmark 脚本都会把 `COMPILER2026_DAG_THREADS`、`COMPILER2026_DAG_PROFILE`、`COMPILER2026_TASK_BATCH`、`COMPILER2026_ASYNC_MIN_B`、`COMPILER2026_ASYNC_MIN_BLOCKS` 透传给 contestant。benchmark 还支持 `COMPILER2026_DAG_THREAD_LIST=1,2,4` 一次扫描多个线程数；CSV 仍用 `threads` 字段区分记录，输出目录按 `threads_<count>` 拆分，terminal summary 也按线程分组。benchmark 脚本会在打开 `COMPILER2026_DAG_PROFILE=1` 时捕获这些 stderr profile 行，并把解析后的 profile 字段写入 benchmark CSV，包括 auto 模式下实际生效的 runtime batch 摘要。
- `submission/scripts/tune_params.sh` 是 benchmark 的离线调参包装：它遍历 `COMPILER2026_TUNE_ASYNC_MIN_B_LIST` 和 `COMPILER2026_TUNE_TASK_BATCH_LIST`，每个组合再交给 `benchmark.sh` 扫 `COMPILER2026_TUNE_THREAD_LIST`，最后汇总为一个 aggregate CSV。该脚本用于真实目标机事前 profile-guided 选择默认阈值，不参与 contestant 计时路径。

Runtime 不包含 `trsm` / `madd` 专用 wrapper，也不直接封装具体算子语义。profile 名称只用于观测输出；ready-queue DAG 只看整数 block key 的 producer/consumer 关系。实际执行仍是调用 Pass 生成的 task function。官方 ABI 调用保留在 Pass 生成的 IR task function 中。`cholesky` 由优化后的 IR 保持原始同步调用。

实验开关 `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1` 会在 Pass 构建 IR 时启用第一版跨 panel DAG：`cholesky` 也被 outline 成 task，`trsm` 依赖 diagonal block 和自身 block 的 latest producer，`madd` 依赖两个 `trsm` block 和自身输出 block 的 latest producer，并把 panel 末尾 wait 降为外层分解循环结束前的 wait。该路径在 4 vCPU VM 上已通过 verifier，但性能暂低于默认 panel-local DAG，因此默认关闭。

## 正确性保证

分块 Cholesky 的依赖关系：

- `trsm(row, panel)` 依赖当前 panel 的 `cholesky(panel, panel)`。
- `madd(row, col, panel)` 依赖对应的 `trsm(row, panel)` 和 `trsm(col, panel)`。
- 下一 panel 的 `cholesky` 依赖前一 panel 的相关 `madd` 更新完成。

Pass 和 runtime 当前共同保证：

- `trsm(row, panel)` 任务提交后，runtime 记录其输出 block key。
- `madd(row, col, panel)` 任务依赖 `trsm(row, panel)` 和 `trsm(col, panel)` 对应的 latest producer；两个依赖完成后才进入 ready queue。
- 所有 `madd` 任务完成后才进入下一 panel，避免跨 panel 依赖恢复不完整时破坏正确性。

该策略是 panel 内 ready-queue DAG，正确性优先。后续可以继续把 panel 末尾 barrier 细化为跨 panel ready-queue DAG。

## 当前限制

- 当前版本仍保留 panel 末尾 barrier，不是完整跨 panel 异步 DAG。
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1` 提供跨 panel DAG 实验入口，但当前单全局队列和完整 live DAG 在 4 vCPU VM 上开销偏高，尚未作为默认提交策略。
- block key 恢复当前支持 strip pointer casts 后的一维 `GEPOperator`，并能递归累加嵌套一维 GEP offset；Pass 会先用 `n` / `b` 从 element offset 恢复 block row/col，再组合成 runtime 现有的一维 key。如果后续 IR 形态变化到多维或无法线性化的地址表达式，Pass 会回退到原 submit/wait 路径。
- 当前默认异步阈值 `b >= 18` 和最小 block 数 2 是公开 4 vCPU VM benchmark 上的经验值；`COMPILER2026_ASYNC_MIN_B`、`COMPILER2026_ASYNC_MIN_BLOCKS` 可用于实验覆盖，`b >= 16` 实验触发过段错误，仍不作为默认。
- 当前 task 批量策略仍是 runtime heuristic；profile 数据已经可观测，离线 sweep wrapper 已能生成跨阈值、batch 和线程数的 aggregate CSV，但 runtime 尚未把历史 profile 自动反馈成下一次默认策略。
- 小 block serial path 能保证不因任务过细而严重退化，但当前 VM 上仍有少量版本化分支开销。

## 本地验证

在 openEuler/BiSheng VM 中执行：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh
```

完整 benchmark：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
LABEL=successor_edge_pool_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

详细性能记录见 `docs/performance.md`。
