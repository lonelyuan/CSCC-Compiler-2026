# 后续优化路线

本文记录面向真实鲲鹏 920 多核平台和决赛扩展用例的长期路线。当前代码已经避免整函数替换，转为 IR-level 算子识别、任务化和保守 DAG 同步；后续重点是把 panel barrier 调度升级为更通用的编译器分析与运行时协同优化。

## 平台假设

当前本地 openEuler VM 只分配 4 个 vCPU，不能代表真实测试平台。鲲鹏 920 常见服务器配置是单路 48/64 核或双路更多核心，内存带宽、NUMA、cache 层次、线程调度开销都和本地 VM 差异很大。

本项目后续不应把 `COMPILER2026_DAG_THREADS=4`、`b >= 32` 等参数视为最终常量，而应把它们作为当前调试平台上的经验值。真实平台上需要按核心数、block size、矩阵规模和算子耗时动态决策。

## 更鲁棒的实验设计

后续 benchmark 应覆盖以下维度：

- 核心数：`1, 2, 4, 8, 16, 32, 48, 64`，真实平台上再加入 NUMA 绑定实验。
- 矩阵规模：公开 `n <= 10000` 范围内分层采样，包括 `n=512/768/1024/2048/4096/8192/10000`。
- block size：覆盖小块、中块和大块，例如 `8, 16, 24, 32, 48, 64, 96, 128, 192, 256`。
- 指标：正确率、几何平均加速比、P50/P95 时间、任务数、队列等待时间、worker 空闲率、主线程 wait 空等时间、每类算子耗时。
- 对比组：官方串行、当前 panel-barrier、不同线程数、不同异步阈值、未来 ready-queue DAG。

结论不要只看单点 speedup，应使用几何平均和分层统计，避免对公开样例或 4 核 VM 过拟合。

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

当前实现已经去掉 `trsm` 阶段全局 wait，让 `madd` 在对应两个 `trsm` 完成后进入 ready queue；panel barrier 完成后 runtime 会清理 panel-local DAG 状态，避免旧 producer 表跨 panel 残留。下一步才是让下一 panel 的关键路径提前启动，不必等待整个 trailing matrix 更新完成；这是大核数平台上最重要的性能空间。

### 3. 运行时任务粒度自适应

单个 `madd` task 对小 `b` 可能过细，对大 `b` 又足够重。运行时应支持：

- 小 task 合并为 range task。
- 大 task 保持单算子粒度以提升负载均衡。
- 根据 `n, b, block_count, thread_count` 自动选择粒度。
- 收集轻量 profile，为下一次调用选择阈值。

当前已具备第一版 profiling 开关：`COMPILER2026_DAG_PROFILE=1` 会输出 async path 判定次数和原因、task 数、队列等待、执行时间、worker idle、主线程 wait 空等、批量出队、DAG 节点/边/释放/fanout/live 信息，以及按 Pass 注册名称聚合的 `trsm/madd` 统计。benchmark 脚本已经能把这些统计沉淀为 CSV 字段。Pass 入口已经改为 runtime predicate，`COMPILER2026_ASYNC_MIN_B` 和 `COMPILER2026_DAG_THREADS` 能真实控制 async path，后续可以继续把这些 profile 数据用于驱动 `COMPILER2026_TASK_BATCH`、默认异步阈值、未来 range task 和跨 panel DAG 的收益判断。

### 4. 多核和 NUMA 亲和性

真实鲲鹏平台上需要测试：

- worker pinning。
- work stealing 和 per-worker deque。
- NUMA-aware task placement。
- 避免单全局队列在 48/64 核上成为瓶颈。

当前单锁队列适合验证 IR pass 方向，但不是大核数最终形态。

当前提交已经先落地了一层轻量缓解：runtime 对小/中等 `b` 使用小批量提交和批量出队，降低公开 VM 上 `madd` 密集阶段的锁竞争。后续在 32 核以上平台仍应继续评估 per-worker deque、work stealing 和 NUMA 绑定。

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
- Pass 已有一版基于一维 `GEPOperator` offset 的 block key 恢复，但还不是通用数组子块坐标和读写集合分析。
- Runtime 仍以单全局队列为核心，虽然已有批量提交/出队缓解，扩展到 32 核以上仍可能出现锁竞争。
- 阈值和 task batch 大小仍来自经验测试；async 入口已由 runtime predicate 按 block size、block count 和 thread count 控制，runtime 和 benchmark 已能记录 profile，但尚未将 profile 闭环成自动 heuristic。

短期目标是把 `trsm/madd` 的坐标和依赖边从 IR 中恢复出来；中期目标是生成 ready-queue DAG；长期目标是把这个 pass 做成可解释、可迁移的 tiled linear algebra taskization pass。
