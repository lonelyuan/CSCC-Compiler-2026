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

### 语义 annotation

提交包在 `src/baseline/block_cholesky.cpp` 的函数定义前增加唯一一条合法标注：

```cpp
[[clang::annotate("compiler2026.graph.block_cholesky.tile_dag.v1")]]
```

去掉该属性后，文件与官方 baseline 完全一致。`tile_dag.v1` 是版本化图区域契约：`L` 是
row-major double 矩阵，`cholesky` 读写对角 tile，`trsm` 读取对角 tile 并读写其输出
tile，`madd` 读取两个 panel tile 并读写输出 tile。它不包含矩阵规模、线程数、平台参数或
测例信息。

Pass 在克隆前从 `llvm.global.annotations` 识别该契约。默认 panel-local 路径继续使用原有
GEP offset 恢复，不改变当前交付行为；仅在跨 panel 实验中，Pass 才依据契约用
`operator_ptr - L_base` 恢复 element offset。这样即使 Clang 把原 GEP 链折叠进 PHI，仍能
得到 `(block_row, block_col)`。标注缺失或版本不匹配时保守回退原分析。

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

默认 predicate 使用 `b >= 12`、block 数大于等于 2 且可用线程数大于 1；`COMPILER2026_ASYNC_MIN_B` 和 `COMPILER2026_ASYNC_MIN_BLOCKS` 可以覆盖阈值，用于 smoke/profile/benchmark 调参。

async clone 是从官方 baseline IR 克隆出来的，不是手写算法替换。

### 算子调用转换

async path 内每个 panel 的调度形状是"cholesky 同步 → trsm 相位 → 屏障 → madd 相位 → 屏障":

- `cholesky(...)` 保持同步原始调用,由主线程执行。
- `trsm(...)` 被 outline 到 Pass 生成的 task function,用**无依赖**的 `runtime_submit` 提交;
  trsm 循环出口插入 `runtime_wait()`。
- `madd(...)` 的**内层 k 循环整体**变成一个 range task,用 `runtime_submit_range` 提交;
  外层 j 循环出口插入 `runtime_wait()`(panel 末尾屏障)。

panel 内不再维护依赖边。依据:同一 panel 内所有 `trsm` 只依赖主线程刚同步执行完的
`cholesky`,彼此独立;所有 `trsm` 完成后,每个 `madd` 写不同的块 `(k, j)`,彼此也独立。
因此相位屏障已经表达了全部依赖,`latest_producer` 表和 DAG 节点在默认路径上是多余开销。
带依赖边的 `submit_deps*` 仅保留给默认关闭的跨 panel 实验路径。

range task 的形状:

```llvm
define internal void @compiler2026_task_madd_range(ptr %ctx, i32 %begin, i32 %end) {
  ; for (t = begin; t < end; ++t)
  ;   madd(A0 + t*b*n, B0, C0 + t*b*n, b, n)
}
```

步长来自源循环里算子自身的实参形态。源循环是
`madd(&L[k*n+i], &L[j*n+i], &L[k*n+j], b, n)`,`k` 每步 `+b`,所以第一、第三个指针每步
前进 `b*n` 个 `double`,第二个不变。**range task 发出的 `madd` 调用、实参和顺序与原循环
完全一致,只有"分到哪个 task"变了**;算子实现、ABI 和调用次数都不变。

三个基址和迭代次数由 `tile_dag.v1` 声明的 row-major `L` 语义恢复,不需要分析 PHI:

```text
elements = (B0 - L) / sizeof(double) = j*n + i     (i < n,故整除得 j)
j        = elements / n
A0 = B0  = &L[j*n + i]        (k = j 时 A 恰好等于 B)
C0       = &L[j*n + j]
count    = (n - j) / b        (k = j, j+b, ... < n)
```

`B0` 是 `madd` 的第二个实参,在 k 循环内是循环不变量,因此可在 preheader 取用。
**不做 CFG 手术**:只删除内层循环里的 `madd` 调用,循环本身此时已无副作用,交给后续 -O2 的
loop deletion 消除;若未消除,代价只是一个空循环,不影响正确性。若 `B0` 不是循环不变量或
取不到 preheader,则逐个 `madd` 回退到原来的 `runtime_submit` 路径。

Pass 根据 `LoopInfo` 插入同步:

- `trsm` 所在 loop 的 exit block 插入 `compiler2026_runtime_wait()`。
- `madd` 内层 loop 的父 loop exit block 插入 `compiler2026_runtime_wait()`。
- 函数 async path 入口插入 `compiler2026_runtime_begin(n, b)`,并注册
  `compiler2026_task_trsm` / `compiler2026_task_madd_range` 的 profile 名称;
  注册只提供观测标签,不改变调度语义。
- async path 返回前插入 `compiler2026_runtime_end()`。

优化后的 IR 仍保留原始 `block_cholesky` 的串行循环结构,并额外生成 async clone。
默认路径上的 IR 断言(`scripts/percase_bench.sh` 会检查):

```text
ir_submits=1 (plain=1 deps=0)   一次 trsm submit;madd 走 submit_range
ir_wait_calls=2                 每 panel 两个屏障
ir_trsm_calls=2  ir_madd_calls=3
```

已验证 IR 片段包含:

```text
define dso_local noundef i32 @_ZN7contest14block_choleskyEPKdPdii(...)
  call i32 @compiler2026_runtime_should_async(...)
  call i32 @compiler2026_async_impl(...)

define internal noundef i32 @compiler2026_async_impl(...)
  call void @compiler2026_runtime_submit(ptr @compiler2026_task_trsm, ptr ...)
  call void @compiler2026_runtime_submit_range(ptr @compiler2026_task_madd_range, ptr ..., i32 %count)
  call void @compiler2026_runtime_wait()

define internal void @compiler2026_task_trsm(ptr ...)
  call void @trsm(...)

define internal void @compiler2026_task_madd_range(ptr ..., i32 ..., i32 ...)
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
extern "C" void compiler2026_runtime_submit_range(
    void (*fn)(void *, int begin, int end), void *ctx, int count);
extern "C" void compiler2026_runtime_submit_deps(
    void (*fn)(void *), void *ctx, int dep_a, int dep_b, int output);
extern "C" void compiler2026_runtime_submit_deps3(
    void (*fn)(void *), void *ctx, int dep_a, int dep_b, int dep_c, int output);
extern "C" void compiler2026_runtime_submit_deps3_priority(
    void (*fn)(void *), void *ctx, int dep_a, int dep_b, int dep_c,
    int output, int priority);
extern "C" void compiler2026_runtime_wait();
extern "C" void compiler2026_runtime_end();
```

Runtime 内部维护一个 thread-local `AsyncRuntime`：

- `runtime_should_async` 集中管理 async path 入口阈值和线程数判断，使 Pass 入口分支、runtime 线程选择和 benchmark 记录的 `async_min_b` / `async_min_blocks` 保持一致。
- `runtime_alloc` 为 Pass 生成的 task context 分配内存。
- `runtime_submit` 只接收 task function 指针和 context 指针。
- `runtime_submit_range` 是通用 parallel-for:接收一个 `fn(ctx, begin, end)`、一个 context 和
  一个 `count`,把 `[0, count)` 切成若干块,每块作为一个 task 调用 `fn(ctx, begin, end)`。
  **runtime 既不解释 ctx,也不解释下标的含义**,只决定切多大——下标语义完全由 Pass 生成的
  range task body 拥有。因此它仍然是通用任务调度接口,不是算子特化。
- 切块长度按"每个 task 恒定工作量"决定:`目标 flops / (2b³)`,即用一个 `madd` 携带的
  `2b³` flops 去除目标预算。同一条规则覆盖两个极端:`b=8` 时一个 task 装下整条 k 循环,
  `b>=64` 时退化为一个 task 一个 `madd`,中间自然插值。`COMPILER2026_RANGE_TASK_FLOPS`
  可覆盖目标预算。这把"粒度自适应"从 Pass 移到了 runtime,Pass 不需要猜 `b`。
- `runtime_submit_deps` 额外接收两个输入 block key 和一个输出 block key；runtime 用这些 key 维护 latest-producer 依赖和 ready queue，但不理解具体算子语义。
- `runtime_submit_deps3` 是三输入依赖版本，用于实验性的跨 panel DAG：`madd` 需要同时依赖两个 `trsm` 输入和自身输出块的 previous producer。
- `runtime_submit_deps3_priority` 是默认关闭的通用 rank 版本。Pass 按 tile 坐标计算
  `0..3` 的关键路径等级；runtime 只按整数等级选择 ready queue，不读取算子名称或矩阵语义。
  rank 使用 `uint8_t` 填入 `DagNode::completed` 后的 padding，64 位节点保持 40 B；开关
  关闭时 enqueue/dequeue 直接走原 normal FIFO fast path。
- `runtime_wait_key` 是实验性跨 panel 路径使用的通用 key wait：给定一个整数 block key，runtime 等待当前 latest producer 完成，等待期间主线程也会执行 ready queue 中的任务。存在 key waiter 时，worker 或提交线程完成 ready batch 后会通知 `done_cv`，避免 key wait 依赖固定超时轮询发现 producer 完成。它不理解 key 对应哪个算子或矩阵块。
- 当前 panel-local DAG 下，Pass 对 `trsm` submit 保留 output key，对 `madd` submit 使用 output key `-1`。原因是 panel 末尾仍有 wait，`madd` 输出在当前 DAG 作用域内没有后续 async consumer；这样可以减少 runtime 对无消费者 `madd` 节点的 `latest_producer_` 哈希表更新。后续做跨 panel DAG 时，需要重新把相关 `madd` 输出纳入依赖图。
- worker 线程从队列中取任务，调用 Pass 生成的 task function。
- `wait` 等待队列为空且所有运行中任务完成；主线程也会参与执行队列任务。
- 当前 DAG 作用域是 panel-local；`wait` 确认 DAG 节点全部完成后会清理已完成节点和 latest-producer 表，profile 计数保留到 `end` 汇报。
- `end` 做最终等待并重置本轮 arena，不销毁可复用 worker 池。
- ready queue、DAG node vector、latest-producer 表和批量提交状态的 reset 在 runtime mutex 下执行，避免 worker 池复用时清理调度状态和 worker 观察队列状态并发。
- 依赖释放和 submit flush 使用 `notifyWorkers(count)` 定向唤醒：按新就绪任务数调用相应次数 `notify_one`，只有 `count` 不小于 worker 数时才退化为 `notify_all`，避免每次释放事件产生 O(participants) 次无效 futex 唤醒。停机路径仍使用 `notify_all`。欠唤醒是安全的：每个参与者完成一批后都会重新求值就绪谓词。

- `COMPILER2026_DAG_PIN_WORKERS=1` 是默认关闭的 Linux worker 亲和性实验：runtime 从当前进程 affinity mask 读取可用 CPU，并把 worker thread 轮转绑定到这些 CPU；非 Linux 或 affinity 读取失败时自动退化为不绑定。该机制只影响线程放置，不改变 DAG 依赖、ready queue 或 task function。
- `COMPILER2026_DAG_WORK_STEALING=1` 是默认关闭的 per-worker deque + work-stealing 实验路径：DAG 在提交阶段单线程构建，drain 阶段用 per-node 原子 pending 计数、不可变 successor 边和分片 deque（本地 LIFO 批量出队 + 跨 shard FIFO 窃取），空闲时有界自旋后 CV 睡眠并以 push 序号防丢唤醒，终止用单原子 outstanding 计数。它只服务默认 panel-barrier 调度；与跨 panel `runtime_wait_key` 同时启用时，`wait_key` 退化为一次完整 drain（保守正确）。默认关闭时仍走当前已验证的持久无锁 phase/全局队列组合路径。当前本机 M5 与早期 4 核 VM 上未超过当时的单队列默认；该原型保留为调度研究入口，不改变 Round 17–27 的默认路线，详见 `docs/engineering_log.md` 与 `tools/sched_harness/`。
- task context 使用 per-call arena 分配，避免每个 task 单独 `malloc/free`。
- runtime 按首个 panel 的 `trsm + madd` 任务数预估 panel-local task 容量，并复用 ready queue、DAG node vector 和 latest-producer hash table 的容量。
- DAG successor 边使用 runtime 统一的连续 edge pool；每个 producer node 只保存 successor 链表的 head/tail 和计数，避免为高 fanout `trsm` producer 维护大量独立小 vector。
- `COMPILER2026_DAG_MAX_LIVE` 是默认关闭的 live-window drain：非零时，DAG submit 发现 live DAG 数量超过窗口且已有 ready task，会由提交线程执行一小批 ready task 后继续提交。该机制只看通用 DAG 状态，不包含算子特化逻辑，主要用于跨 panel DAG 实验降低完整图的 live pressure。
- 依赖感知的 submit 采用"提交者暂存 + 批量发布"：依赖键解析、去重和输出键登记在临界区外完成（`dag_nodes_`、`successor_edges_`、`latest_producer_` 只有提交线程追加），`flushDagStaging()` 在一次加锁内按"先追加全部节点 → 再连全部边 → 最后入队就绪节点"的顺序发布。批量大小为 `reserve_tasks / (participants * 2)`，上限 32，`COMPILER2026_DAG_SUBMIT_BATCH` 可覆盖；block 数少时自动退回逐个发布。这解决了"单线程提交者与 N 个 worker 争同一把锁"导致的队列饥饿正反馈。
- 参与者数为 `min(可用线程, participantCapForTile(b))`。`participantCapForTile` 现在只有
  一档:`b < 48 → 24`,否则不限。`COMPILER2026_DAG_PARTICIPANT_CAP` 可覆盖(正整数强制、
  `off` 关闭)。此前基于 block 数的两个界(`min(threads, blocks)` 和平均 panel 宽度界)已被
  实测反例删除:一个 panel 有多达 `blocks*(blocks-1)/2` 个 `madd`,可用并行度不受 panel 数
  限制(`b=128` 只有 9 个 block,却从 8 参与者的 5.45x 涨到 12 参与者的 5.71x)。
  **24 这个常数是平台相关的经验值,换平台必须用 `scripts/participant_sweep.sh` 重测。**
- 最小工作量门槛:`n < 192`、或 `b < 12 且 n < 320` 时直接走串行路径。固定开销是每 panel
  两个屏障(共 `2n/b` 个),工作量太小时摊不平。这两个界也是本机实测交叉点,换平台需重测。
- 提交侧暂存深度为 `参与者数 * 8`(上限 4096),与出队批量上限是**两个独立的量**。
  暂存深度必须随参与者数缩放:固定 16 的暂存喂不满 36 个等待中的 worker,队列建立不起深度,
  批量出队会塌到 1,于是"加核变慢"。`chooseBatchCount` 在 `available <= participants*2` 时
  返回 1,这使问题在 36 参与者下是结构性的。
- worker 池按进程内最大需求建一次,**只增不减**;`resetForCall` 设置本次调用的活跃 worker 数,
  序号超出的 worker 挂在**独立的 `park_cv_`** 上。用独立 condition variable 是必须的:
  若挂起的 worker 也等在 `work_cv_` 上,它可能吃掉一次本该唤醒活跃 worker 的 `notify_one`。
  这样避免了"参与者数在 24 和满线程之间变化就重建整个池"——一次重建约 2.4ms,
  比最小用例的整个串行时间还长。
- 库加载时(`PoolPrewarm`)建好线程池并跑一次**丢弃的空并行域**,把 arena 首个 chunk、
  队列 vector 首次分配、每个 worker 的首次 futex 唤醒和栈缺页全部走热。线程池是进程级资源,
  惰性构造会把这笔一次性开销整个记到"第一个走异步路径的用例"头上,而它的串行时间可能只有
  几百微秒。惰性构造仍保留作为兜底,初始化没跑到只会更慢、不会更错。
- 批量出队大小只按 tile 粒度选择(`b <= 64 → 16`,`b <= 128 → 8`,更大 → 1;上限 `kMaxTaskBatch = 32`);公平性由 `chooseBatchCount()` 动态保证——ready 宽度不足时返回 1,否则最多发放 `available / participants`。此前基于 block 数的静态钳制已移除,因为它在参与者数接近 block 数时会错误地把批量压成 1。
- `COMPILER2026_TASK_BATCH` 可覆盖默认批量大小，用于真实多核平台调参。
- `COMPILER2026_DAG_PROFILE=1` 打开轻量 profiling，向 stderr 输出 async path 判定次数和原因、任务数、队列等待时间、执行时间、worker idle 时间、`wait()` 调用次数和总耗时、`wait()` 入口 ready/active/DAG live pressure、主线程在 `wait()` 中无 ready task 可执行的等待时间、批量出队信息、ready queue 宽度采样、DAG 节点/边/已满足依赖/缺失依赖/first-touch 输入依赖/释放批量/fanout/live 统计，以及按已注册 task 名称聚合的 `trsm/madd` 统计。
- smoke 和 benchmark 脚本都会把 `COMPILER2026_DAG_THREADS`、`COMPILER2026_DAG_PROFILE`、`COMPILER2026_TASK_BATCH`、`COMPILER2026_ASYNC_MIN_B`、`COMPILER2026_ASYNC_MIN_BLOCKS`、`COMPILER2026_DAG_MAX_LIVE`、`COMPILER2026_DAG_PIN_WORKERS`、`COMPILER2026_DAG_CRITICAL_PRIORITY` 透传给 contestant。benchmark 还支持 `COMPILER2026_DAG_THREAD_LIST=1,2,4` 一次扫描多个线程数；CSV 仍用 `threads` 字段区分记录，并用 `dag_pin_workers` 记录 worker 亲和性实验开关，同时用 `pass_cross_panel_dag` 和 `pass_sync_cholesky` 固化两个决定 IR 的 Pass 开关。输出目录按 `threads_<count>` 拆分，terminal summary 也按线程分组。benchmark 脚本会在打开 `COMPILER2026_DAG_PROFILE=1` 时捕获这些 stderr profile 行，并把解析后的 profile 字段写入 benchmark CSV，包括 auto 模式下实际生效的 runtime batch 摘要。成功完成后默认删除大体量 per-suite 输入/输出/profile 目录，只保留 CSV、IR 和二进制；`COMPILER2026_BENCH_KEEP_ARTIFACTS=1` 可保留这些调试文件。
- `submission/scripts/tune_params.sh` 是 benchmark 的离线调参包装：它遍历 `COMPILER2026_TUNE_ASYNC_MIN_B_LIST`、`COMPILER2026_TUNE_ASYNC_MIN_BLOCKS_LIST`、`COMPILER2026_TUNE_TASK_BATCH_LIST`、`COMPILER2026_TUNE_DAG_MAX_LIVE_LIST` 和 `COMPILER2026_TUNE_DAG_PIN_WORKERS_LIST`，每个组合再交给 `benchmark.sh` 扫 `COMPILER2026_TUNE_THREAD_LIST`，最后汇总为一个 aggregate CSV。该脚本用于真实目标机事前 profile-guided 选择默认阈值、实验性 live-window 和 worker 亲和性，不参与 contestant 计时路径。

Runtime 不包含 `trsm` / `madd` 专用 wrapper,也不直接封装具体算子语义。profile 名称只用于
观测输出。默认路径上 runtime 只做三件事:执行 Pass 生成的 task function、把
`[0, count)` 切块、以及在 `wait()` 处做屏障——它不知道 task 在算什么,也不知道下标对应哪个块。
带 block key 的 ready-queue DAG 只在默认关闭的跨 panel 实验路径上使用,且同样只看整数 key 的
producer/consumer 关系。官方 ABI 调用全部保留在 Pass 生成的 IR task function 中,
`cholesky` 由优化后的 IR 保持原始同步调用。

**约束遵守**:算子实现 `cholesky` / `trsm` / `madd` 未被修改、替换、重定义或绕过;
算法源码 `src/baseline/block_cholesky.cpp` 除一行 `[[clang::annotate]]` 标注外与官方版本
逐字节相同(标注语义见"语义 annotation"一节);性能提升全部来自 Pass 的依赖分析
(panel 内相位独立性)和 runtime 的并行执行与粒度选择。range task 改变的只是"哪些 `madd`
调用归到同一个 task",不改变调用的实参、次数和顺序。

实验开关 `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1` 会在 Pass 构建 IR 时启用第一版跨 panel DAG：`cholesky` 也被 outline 成 task，`trsm` 依赖 diagonal block 和自身 block 的 latest producer，`madd` 依赖两个 `trsm` block 和自身输出 block 的 latest producer，并把 panel 末尾 wait 降为外层分解循环结束前的 wait。该路径在 4 vCPU VM 上已通过 verifier；配合 `COMPILER2026_DAG_MAX_LIVE=2048` 可把 profile 中的 `max_dag_live` 从早先完整图约 `6072` 降到约 `2050`，但性能仍暂低于默认 panel-local DAG，因此默认关闭。

进一步的实验开关 `COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1` 需要和 `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1` 一起使用。该路径不再 taskize `cholesky`，而是在原始同步 `cholesky` 调用前插入 `runtime_wait_key(diagonal_input_key)`；`trsm` 和 `madd` 仍使用跨 panel dependency submit。这样保留跨 panel 的 producer/consumer 表达能力，同时减少额外 cholesky task、cholesky task context 和相关 DAG live 压力。它仍是实验路径，默认关闭。

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

- 默认路径仍是 panel 局部调度:每个 panel 两个相位屏障,不是完整跨 panel 异步 DAG。
  跨 panel 已被**两次否证**(见 `docs/engineering_log.md` Round 1 与 Round 7):panel 内
  ready 宽度实测可达 1134,而可持续参与者数在 24 量级,可用并行度比参与者数高一到两个
  数量级,因此"增加可用并行度"的结构性改动不可能有收益。`COMPILER2026_ENABLE_CROSS_PANEL_DAG=1`
  等实验入口仍保留,但不应作为优化方向。
- 参与者上限 24、最小工作量门槛(`n < 192`、`b < 12 且 n < 320`)、range task 的 50000 flops
  预算,**全部是 40 物理核 x86 调试机上的实测经验值**,不是理论常数。目标平台是鲲鹏 920,
  核数、内存带宽和单核算子吞吐都不同,必须用 `scripts/participant_sweep.sh` 和
  `COMPILER2026_RANGE_TASK_FLOPS` 重测。aarch64 单核向量宽度更窄,同一 `b` 的 tile task
  更长,因此这些界在目标平台上只会更宽松——误差方向是安全的。
- block key 恢复当前支持 strip pointer casts 后的一维 `GEPOperator`,并能递归累加嵌套一维
  GEP offset;range task 的基址恢复依赖 `tile_dag.v1` 声明的 row-major `L` 语义和指针差。
  若 IR 形态变化到无法线性化的地址表达式,Pass 会逐个 `madd` 回退到原 submit/wait 路径。
- 剩余损失已定量但未解决(`n=1152 b=16`,20 参与者):worker 空等约占 wall 的 42%,
  task 自身执行时间比串行膨胀 1.33x(小 tile 在核间迁移的 cache 代价)。前者是相位屏障的
  尾部不齐,后者不是调度问题。
- 等权重指标下的结构上界:`n <= 256` 的 18 个用例实测最好只有 1.0–2.5x(总工作量太小,
  40 核上摊不平固定开销),它们封住了 geomean 的上限。用"每个用例都达到该 `n` 下已观测最好
  成绩"的乐观假设合成,geomean 约 7.46x。
- 调参已受测量能力限制:同一配置连跑两遍全量,geomean 分别为 4.017x 和 4.247x(差 5.7%),
  因此 3% 以内的改动无法用聚合数字判断,必须做配对逐用例对照。

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
LABEL=live_window_default_repeat3_final REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

详细性能记录见 `docs/performance.md`。
