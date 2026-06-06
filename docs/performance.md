# 性能记录

本文件记录当前正式 IR-level Pass 的性能。所有数据来自 openEuler aarch64 VM，使用毕昇 3.2.0.1 / LLVM 15.0.4，`COMPILER2026_DAG_THREADS=4`，每组重复 3 次。

## 当前有效结果

当前交付版本是 `runtime_ready_queue_trsm_deps`：

```text
docs/benchmark_results/runtime_ready_queue_trsm_deps.csv
```

VM 原始输出：

```text
/root/bisheng/build/optimization_benchmarks/runtime_ready_queue_trsm_deps.csv
```

结果摘要：

| Suite | serial avg | contestant avg | speedup |
| --- | ---: | ---: | ---: |
| `n512_576` | 0.089390s | 0.072622s | 1.232x |
| `n768` | 0.228592s | 0.126365s | 1.809x |
| `n1024` | 0.308963s | 0.165681s | 1.865x |
| `n1152_small_b` | 0.369142s | 0.287641s | 1.283x |

四个 suite 平均加速比的几何平均约为 `1.520x`。所有 contestant 输出均通过 verifier。

## 本轮优化变化

当前版本保留 IR-level 算子任务化路线，但做了以下 runtime/阈值优化：

- async 阈值从 `b >= 64` 调整为 `b >= 32`，并确保 Pass 入口分支和 runtime 默认阈值一致。
- Pass 入口分支改为调用 `compiler2026_runtime_should_async(n, b)`，使 `COMPILER2026_ASYNC_MIN_B` 和 `COMPILER2026_DAG_THREADS` 不只是 CSV 元数据，而是实际控制 async path 是否启用。
- runtime 由每次 `block_cholesky` 调用创建/销毁 worker 改为 thread-local worker 池复用，worker 数变化时才重建。
- task context 改为 arena 分配，避免每个 `trsm/madd` task 单独 `malloc/free`。
- ready queue、DAG node vector 和 latest-producer hash table 按首个 panel 的 `trsm + madd` 任务数预估容量并跨调用复用，降低 panel-local DAG 构建时的扩容/rehash 开销。
- `wait()` 中主线程参与执行队列任务，使配置的线程数近似为 `main + workers`。
- 任务队列从 `deque` 改为可复用 vector 队列，并按当前 block 数预留容量。
- 提交端减少重复 `notify_one`，降低大量小 task 入队时的条件变量通知开销。
- 小/中等 `b` 的 task 在提交端按小批量 flush，worker/main 在队列积压时批量出队执行，降低 `madd` 密集阶段的锁竞争。
- 批量大小按 `b` 分级选择，并可用 `COMPILER2026_TASK_BATCH` 在真实平台上覆盖调参。
- 新增 `COMPILER2026_DAG_PROFILE=1` 观测模式，默认关闭；打开后 runtime 会向 stderr 输出 async path 判定次数和原因、task 数、队列等待、执行时间、worker idle、批量出队、DAG fanout，以及按 Pass 注册名称聚合的 `trsm/madd` 统计。
- Pass 从 `trsm/madd` 的一维 `GEPOperator` offset 恢复一版 block key，调用 `compiler2026_runtime_submit_deps` 把依赖交给 runtime。
- runtime 增加通用 ready-queue DAG：`madd(k,j,p)` 依赖对应两个 `trsm(k,p)` / `trsm(j,p)` 输出，`trsm` 阶段不再使用全局 wait；panel 末尾仍保留 wait，暂不跨 panel 调度。

`b >= 16` 也做过实验，但在公开 benchmark 中触发段错误，已回退，不作为可交付配置。

profile 示例命令：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
SPEC_START=93 SPEC_END=93 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/smoke_test.sh
```

benchmark 中打开同一个环境变量时，脚本会把 profile stderr 捕获到每个 suite 目录下的 `contestant_<run>.profile`，并把解析后的字段写入 CSV：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
COMPILER2026_DAG_PROFILE=1 LABEL=ready_queue_profile_csv_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

新增 CSV 字段包括 `task_batch`、`async_min_b`、`ir_submit_deps`、`ir_submit_plain`、`ir_trsm_calls`、`ir_madd_calls`、`async_decisions`、`async_enabled`、`async_disabled`、`async_disabled_small_b`、`async_disabled_threads`、`async_disabled_single_block`、`profile_calls`、`total_tasks`、`main_tasks`、`worker_tasks`、`dag_nodes`、`dag_edges`、`dag_initial_ready`、`dag_released`、`max_dag_pending`、`max_dag_successors`、`queue_ms`、`exec_ms`、`worker_idle_ms`、`trsm_count`、`madd_count` 等。每个 suite/run 可能包含多个矩阵调用，benchmark 会把同一次运行里的 IR 计数和 profile 行聚合到同一条 CSV 记录。默认不打开 profile 时动态 profile 字段为 `0`，计时 CSV 结构保持一致。benchmark 终端摘要同时输出 IR call site 计数、所有行的 `serial_total`、`contestant_total`、算术平均 speedup 和几何平均 speedup，避免后续调参只看单 suite 或手算几何平均。

示例输出节选：

```text
[compiler2026_profile] n=1024 b=32 threads=4 workers=3 batch=16 tasks=5952 ...
[compiler2026_profile_task] name=trsm count=496 ...
[compiler2026_profile_task] name=madd count=5456 ...
```

## 基准组

`submission/scripts/benchmark.sh` 默认跑以下公开用例区间：

| Suite | 公开规格行 |
| --- | --- |
| `n512_576` | 43-56 |
| `n768` | 71-81 |
| `n1024` | 91-96 |
| `n1152_small_b` | 97-104 |

运行命令：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
LABEL=runtime_ready_queue_trsm_deps REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

## 历史结果说明

仓库中还保留了以下 CSV：

```text
docs/benchmark_results/before_runtime_opt.csv
docs/benchmark_results/after_runtime_opt.csv
docs/benchmark_results/after_madd_coarsening.csv
docs/benchmark_results/ir_loop_pass_final.csv
docs/benchmark_results/ir_outlined_task_pass.csv
docs/benchmark_results/ir_async_threshold32.csv
docs/benchmark_results/runtime_submit_dequeue_batch.csv
docs/benchmark_results/runtime_ready_queue_trsm_deps.csv
docs/benchmark_results/profile_csv_smoke.csv
docs/benchmark_results/ready_queue_profile_csv_smoke.csv
docs/benchmark_results/dag_profile_counters_smoke.csv
docs/benchmark_results/ready_queue_batch8_repeat3.csv
docs/benchmark_results/panel_dag_cleanup_profile_smoke.csv
docs/benchmark_results/async_predicate_profile_smoke.csv
docs/benchmark_results/async_predicate_disabled_smoke.csv
docs/benchmark_results/async_predicate_threads1_smoke.csv
docs/benchmark_results/async_decision_profile_smoke.csv
docs/benchmark_results/async_decision_threads1_smoke.csv
docs/benchmark_results/benchmark_overall_summary_smoke.csv
docs/benchmark_results/dag_successor_fanout_smoke.csv
docs/benchmark_results/gep_operator_key_smoke.csv
docs/benchmark_results/ir_submit_counts_smoke.csv
docs/benchmark_results/dag_reserve_structures_smoke.csv
docs/benchmark_results/panel_task_reserve_smoke.csv
docs/benchmark_results/queue_reset_lock_smoke.csv
```

前三个 CSV 来自早期“整函数替换为 runtime 入口”的实验版本。它们的性能更高，但该路线不够符合赛题对 IR 层算子依赖分析的要求，因此不作为当前提交方案。
`profile_csv_smoke.csv`、`ready_queue_profile_csv_smoke.csv`、`dag_profile_counters_smoke.csv`、`panel_dag_cleanup_profile_smoke.csv`、`async_predicate_profile_smoke.csv`、`async_predicate_disabled_smoke.csv`、`async_predicate_threads1_smoke.csv`、`async_decision_profile_smoke.csv`、`async_decision_threads1_smoke.csv`、`benchmark_overall_summary_smoke.csv`、`dag_successor_fanout_smoke.csv`、`gep_operator_key_smoke.csv`、`ir_submit_counts_smoke.csv`、`dag_reserve_structures_smoke.csv`、`panel_task_reserve_smoke.csv` 和 `queue_reset_lock_smoke.csv` 是 profile 数据链验证用的单次重复实验，用于确认 CSV 字段、聚合逻辑、阈值开关、线程数开关、async decision 原因聚合、整体 summary 输出、DAG successor fanout 统计、block key 恢复 smoke 行为、IR call site 计数、DAG reserve 行为、panel task reserve 估算和 runtime reset 加锁后的 profile 链路，不作为正式性能均值。`ready_queue_batch8_repeat3.csv` 是 task batch 调参对照，当前只作为经验记录，不替代默认配置。

## 结论

当前方案是 panel 内 ready-queue DAG：

- Pass 分析官方 baseline IR 中的 `trsm/madd` call 和 loop exit。
- 原始 `block_cholesky` 保留为小 block 串行路径。
- async clone 中的 `trsm/madd` call site 被替换为通用任务提交。
- `madd` 任务通过 block key 依赖对应两个 `trsm` 输出，runtime 在依赖满足时放入 ready queue。
- panel 末尾仍保留 wait，下一 panel 暂不提前启动。
- Pass 生成的 task function 内直接调用官方 `trsm/madd` ABI。
- runtime 不包含算子专用 wrapper，不替换官方算子实现。

后续更大的性能空间来自跨 panel 的 block-coordinate ready queue DAG：让下一 panel 在其依赖 block 更新完成后提前启动，而不是等待整个 trailing matrix 更新完成。
