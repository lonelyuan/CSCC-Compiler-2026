# 后续优化路线

本文记录面向真实鲲鹏 920 多核平台和决赛扩展用例的长期路线。当前代码已经避免整函数替换，转为 IR-level 算子识别、任务化和保守 DAG 同步；后续重点是把 panel barrier 调度升级为更通用的编译器分析与运行时协同优化。

## 平台假设

当前本地 openEuler VM 只分配 4 个 vCPU，不能代表真实测试平台。鲲鹏 920 常见服务器配置是单路 48/64 核或双路更多核心，内存带宽、NUMA、cache 层次、线程调度开销都和本地 VM 差异很大。

本项目后续不应把 `COMPILER2026_DAG_THREADS=4`、`b >= 18` 等参数视为最终常量，而应把它们作为当前调试平台上的经验值。真实平台上需要按核心数、block size、矩阵规模和算子耗时动态决策。

## 更鲁棒的实验设计

后续 benchmark 应覆盖以下维度：

- 核心数：`1, 2, 4, 8, 16, 32, 48, 64`，真实平台上再加入 NUMA 绑定实验。
- 矩阵规模：公开 `n <= 10000` 范围内分层采样，包括 `n=512/768/1024/2048/4096/8192/10000`。
- block size：覆盖小块、中块和大块，例如 `8, 16, 24, 32, 48, 64, 96, 128, 192, 256`。
- 指标：正确率、几何平均加速比、P50/P95 时间、任务数、队列等待时间、worker 空闲率、主线程 wait 空等时间、每类算子耗时。
- 对比组：官方串行、当前 panel-barrier、不同线程数、不同异步阈值、未来 ready-queue DAG。

结论不要只看单点 speedup，应使用几何平均和分层统计，避免对公开样例或 4 核 VM 过拟合。

当前 `benchmark.sh` 已支持 `COMPILER2026_DAG_THREAD_LIST=1,2,4` 这类多线程扫参入口，并按线程分组输出 summary；terminal summary 现在同时给出 speedup P50/P95，便于先看分布再决定是否复测。成功运行后默认清理 per-suite 临时大文件，避免长时间 sweep 撑满 VM 磁盘。`tune_params.sh` 在其外层补充 `async_min_b × task_batch` 离线扫参，并汇总 aggregate CSV。真实鲲鹏平台上可直接把线程列表扩展到 `1,2,4,8,16,32,48,64`，再结合 profile CSV 的 ready width、wait pressure 和 queue/exec 时间判断默认阈值是否需要重设。

## Pass 演进方向

### 1. IR 层 block-coordinate 识别

当前 Pass 只识别 `trsm/madd` 调用和 loop exit。下一步应从 GEP、induction variable、`n/b` 等表达式中恢复 block 坐标：

```text
trsm(row, panel)
madd(row, col, panel)
cholesky(panel, panel)
```

恢复坐标后，Pass 可以生成更精确的依赖边，而不是只在 loop exit 插入全局 barrier。

### 2. Ready-Queue DAG

上一版依赖模型：

```text
cholesky(panel) -> all trsm(panel) -> all madd(panel) -> next panel
```

当前已经落地一层 panel 内 ready queue：

```text
trsm(r, p) has no async predecessor after cholesky(p)
madd(r, c, p) depends on trsm(r, p), trsm(c, p)
panel end still waits before cholesky(p+1)
```

更完整的模型：

```text
trsm(r, p) depends on cholesky(p)
madd(r, c, p) depends on trsm(r, p), trsm(c, p)
cholesky(p+1) depends only on updates to block (p+1, p+1)
trsm(r, p+1) depends on updates to block (r, p+1)
```

当前实现已经去掉 `trsm` 阶段全局 wait，让 `madd` 在对应两个 `trsm` 完成后进入 ready queue；`madd` submit 在当前 panel-local 作用域中使用 output key `-1`，避免为 panel 内无消费者的输出更新 producer 表。panel barrier 完成后 runtime 会清理 panel-local DAG 状态，避免旧 producer 表跨 panel 残留。下一步才是让下一 panel 的关键路径提前启动，不必等待整个 trailing matrix 更新完成；这是大核数平台上最重要的性能空间。

已有第一版跨 panel DAG 实验入口：设置 `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1` 时，Pass 会 taskize `cholesky`，让 `trsm` 依赖 diagonal/latest output producer，让 `madd` 通过 `runtime_submit_deps3` 依赖两个 `trsm` 和自身输出块 previous producer，并把静态 panel wait 降到外层分解循环结束。该路径证明当前框架可以表达跨 panel 依赖，但在 4 vCPU VM 上 `max_dag_live`、依赖边数和 queue 时间显著上升，暂不作为默认。`COMPILER2026_DAG_MAX_LIVE` 已提供一版默认关闭的 live-window drain，可把跨 panel profile 中的 `max_dag_live` 压到窗口附近；当前最佳窗口 smoke 仍未超过默认 panel-local 结果，后续应继续做 per-worker queue/work stealing 或关键块优先策略，而不是直接默认启用完整跨 panel DAG。

### 3. 运行时任务粒度自适应

单个 `madd` task 对小 `b` 可能过细，对大 `b` 又足够重。运行时应支持：

- 小 task 合并为 range task。
- 大 task 保持单算子粒度以提升负载均衡。
- 根据 `n, b, block_count, thread_count` 自动选择粒度。
- 收集轻量 profile，为下一次调用选择阈值。

当前已具备第一版 profiling 开关：`COMPILER2026_DAG_PROFILE=1` 会输出 async path 判定次数和原因、task 数、队列等待、执行时间、worker idle、`wait()` 调用次数和总耗时、`wait()` 入口 ready/active/DAG live pressure、主线程 wait 空等、批量出队、ready queue 宽度采样、DAG 节点/边/已满足依赖/缺失依赖/释放批量/fanout/live 信息，以及按 Pass 注册名称聚合的 `trsm/madd` 统计。benchmark 脚本已经能把这些统计沉淀为 CSV 字段，并派生 `ready_avg` / `ready_per_thread` 观察 ready 宽度是否足以覆盖线程数，派生 `wait_ready_avg` / `wait_active_avg` / `wait_dag_live_avg` 观察 panel barrier 入口仍有多少可执行或未完成工作。Pass 入口已经改为 runtime predicate，`COMPILER2026_ASYNC_MIN_B`、`COMPILER2026_ASYNC_MIN_BLOCKS` 和 `COMPILER2026_DAG_THREADS` 能真实控制 async path；默认 task batch 也已开始参考 `b`、block 数和线程数，避免小 panel 过度批量出队。后续可以继续把这些 profile 数据用于驱动默认异步阈值、未来 range task 和跨 panel DAG 的收益判断。

短期调参策略是“事前离线搜索，运行时廉价决策”：`tune_params.sh` 负责在目标机器上系统扫阈值、batch 和线程数；contestant 运行时只根据 `n, b, block_count, thread_count` 做 deterministic heuristic，不在被计时执行中做 warmup 或多参数计时搜索。

### 4. 多核和 NUMA 亲和性

真实鲲鹏平台上需要测试：

- worker pinning。
- work stealing 和 per-worker deque。
- NUMA-aware task placement。
- 避免单全局队列在 48/64 核上成为瓶颈。

当前单锁队列适合验证 IR pass 方向，但不是大核数最终形态。DAG successor 边已经从每个 node 一个小 vector 改为统一 edge pool，减少了 fanout 边的小分配；这降低了当前全局队列模型的构图开销，但不替代 per-worker deque/work stealing。

当前提交已经先落地了一层轻量缓解：runtime 对小/中等 `b` 使用默认上限为 `8` 的小批量提交和批量出队，降低公开 VM 上 `madd` 密集阶段的锁竞争。后续在 32 核以上平台仍应继续评估 per-worker deque、work stealing 和 NUMA 绑定。

## 需要调研的方向

优先调研以下关键词和系统：

- Task DAG scheduling for tiled Cholesky factorization。
- PLASMA / QUARK tiled linear algebra runtime。
- StarPU heterogeneous runtime and codelet scheduling。
- PaRSEC dynamic task discovery。
- OpenMP task dependency `depend(in/out/inout)` lowering。
- LLVM Polly / MLIR affine dependence analysis。
- LLVM Tapir / Cilk-style task parallel IR。
- Work stealing deque, Chase-Lev deque, Cilk runtime scheduling。
- NUMA-aware task scheduling on ARM servers。

这些方向和本赛题精神一致：不是替换算法，而是在编译器层恢复依赖、生成任务图、选择合理 runtime 执行策略。

## 当前瓶颈

- Panel 末尾 barrier 仍过保守，限制大核数可扩展性。
- Pass 已有一版基于一维 `GEPOperator` offset 的 block row/col 恢复，并支持递归累加嵌套一维 GEP offset；runtime 仍接收由 row/col 组合出的线性 key，还不是通用数组子块坐标和读写集合分析。
- Runtime 仍以单全局队列为核心，虽然已有批量提交/出队和连续 successor edge pool 缓解，扩展到 32 核以上仍可能出现锁竞争。
- 跨 panel DAG 已有 opt-in 实验路径，但需要继续处理 first-touch 依赖统计、live DAG 内存/锁开销和队列扩展性，不能只靠移除 panel wait 直接默认启用。
- live-window drain 能缓解跨 panel DAG 的 live pressure，但没有解决单全局队列、依赖维护和关键路径优先级问题。
- 阈值仍来自经验测试；async 入口已由 runtime predicate 按 block size、最小 block count 和 thread count 控制，默认 task batch 已开始参考 block count/thread count，runtime 和 benchmark 已能记录 profile，`tune_params.sh` 已能生成离线 aggregate CSV，但尚未形成跨运行的自动 profile-guided heuristic。

短期目标是把 `trsm/madd` 的坐标和依赖边从 IR 中恢复出来；中期目标是生成 ready-queue DAG；长期目标是把这个 pass 做成可解释、可迁移的 tiled linear algebra taskization pass。
