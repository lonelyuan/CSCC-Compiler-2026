# 性能记录

本文件记录当前正式 IR-level Pass 的性能。所有数据来自 openEuler aarch64 VM，使用毕昇 3.2.0.1 / LLVM 15.0.4，`COMPILER2026_DAG_THREADS=4`，每组重复 3 次。

## 当前有效结果

当前交付版本是 `live_window_default_repeat3_final`：

```text
docs/benchmark_results/live_window_default_repeat3_final.csv
```

VM 原始输出：

```text
/root/bisheng/build/optimization_benchmarks/live_window_default_repeat3_final.csv
```

结果摘要：

| Suite | serial avg | contestant avg | speedup |
| --- | ---: | ---: | ---: |
| `n512_576` | 0.089092s | 0.064921s | 1.372x |
| `n768` | 0.230897s | 0.119694s | 1.929x |
| `n1024` | 0.309757s | 0.164806s | 1.880x |
| `n1152_small_b` | 0.364400s | 0.240526s | 1.515x |

四个 suite / 3 次重复的 speedup 几何平均约为 `1.657x`，总耗时 speedup 约为 `1.685x`，`contestant_total=1.769841s`。所有 contestant 输出均通过 verifier。该结果是当前 IR-level、官方 ABI 保留、panel-local ready queue DAG 路线下的有效性能记录；早期整函数替换路线的更高结果不作为当前提交方案。

## 本轮优化变化

当前版本保留 IR-level 算子任务化路线，但做了以下 runtime/阈值优化：

- async 阈值从 `b >= 64` 经 `b >= 32`、`b >= 24` 调整为当前默认 `b >= 18`，并确保 Pass 入口分支、runtime 默认阈值和 smoke/benchmark 默认元数据一致。
- Pass 入口分支改为调用 `compiler2026_runtime_should_async(n, b)`，使 `COMPILER2026_ASYNC_MIN_B`、`COMPILER2026_ASYNC_MIN_BLOCKS` 和 `COMPILER2026_DAG_THREADS` 不只是 CSV 元数据，而是实际控制 async path 是否启用。
- runtime 由每次 `block_cholesky` 调用创建/销毁 worker 改为 thread-local worker 池复用，worker 数变化时才重建。
- task context 改为 arena 分配，避免每个 `trsm/madd` task 单独 `malloc/free`。
- ready queue、DAG node vector 和 latest-producer hash table 按首个 panel 的 `trsm + madd` 任务数预估容量并跨调用复用，降低 panel-local DAG 构建时的扩容/rehash 开销。
- DAG successor 关系从每个 node 一个独立 `std::vector` 改为 runtime 统一的连续 edge pool，保留每个 producer 的 successor 链表头尾，减少 `trsm` fanout 场景下的小 vector 分配和扩容。
- `wait()` 中主线程参与执行队列任务，使配置的线程数近似为 `main + workers`。
- 任务队列从 `deque` 改为可复用 vector 队列，并按当前 block 数预留容量。
- 提交端减少重复 `notify_one`，降低大量小 task 入队时的条件变量通知开销。
- 小/中等 `b` 的 task 在提交端按小批量 flush，worker/main 在队列积压时批量出队执行，降低 `madd` 密集阶段的锁竞争。
- 批量大小按 `b` 分级选择；当前默认把 `b <= 64` 的批量上限设为 `8`，再按 block 数和线程数自动收窄。`COMPILER2026_TASK_BATCH` 仍可在真实平台上覆盖调参。
- 新增 `COMPILER2026_DAG_PROFILE=1` 观测模式，默认关闭；打开后 runtime 会向 stderr 输出 async path 判定次数和原因、task 数、队列等待、执行时间、worker idle、wait 入口 ready/active/DAG live pressure、主线程 wait 空等、批量出队、DAG dependency state、fanout/live，以及按 Pass 注册名称聚合的 `trsm/madd` 统计。
- Pass 从 `trsm/madd` 的一维 `GEPOperator` offset 恢复一版 block row/col；嵌套一维 GEP 会先递归累加 element offset，再组合成 runtime 现有的一维 key，调用 `compiler2026_runtime_submit_deps` 把依赖交给 runtime。
- runtime 增加通用 ready-queue DAG：`madd(k,j,p)` 依赖对应两个 `trsm(k,p)` / `trsm(j,p)` 输出，`trsm` 阶段不再使用全局 wait；panel 末尾仍保留 wait，暂不跨 panel 调度。
- 当前 panel-local DAG 中 `madd` 输出在 panel 末尾 wait 前没有后续 async consumer，Pass 因此把 `madd` submit 的 output key 设为 `-1`，避免 runtime 为这些无消费者节点更新 `latest_producer_` 哈希表；`trsm` output key 仍保留，用于释放对应 `madd`。
- 新增 opt-in 跨 panel DAG 实验路径：`COMPILER2026_ENABLE_CROSS_PANEL_DAG=1` 时，Pass 会 taskize `cholesky`，使用三依赖 submit 表达 `madd` 的两个 `trsm` 输入和自身输出块 previous producer，并把静态 panel wait 降为外层分解循环结束前的 wait。该路径暂不默认启用。
- 新增 opt-in live-window drain：`COMPILER2026_DAG_MAX_LIVE=<N>` 非零时，DAG submit 会在 live DAG 超过窗口且已有 ready task 时由提交线程执行一小批 ready task，用于跨 panel DAG 实验降低完整图 live pressure；默认值为 `0`，panel-local 默认路径不启用。

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

`smoke_test.sh` 和 `benchmark.sh` 都会透传 `COMPILER2026_DAG_THREADS`、`COMPILER2026_DAG_PROFILE`、`COMPILER2026_TASK_BATCH`、`COMPILER2026_ASYNC_MIN_B`、`COMPILER2026_ASYNC_MIN_BLOCKS`。因此小范围 verifier smoke 可以直接验证 async 阈值、线程数、profile 和 batch 覆盖是否真实生效。

`benchmark.sh` 还支持用 `COMPILER2026_DAG_THREAD_LIST=1,2,4` 在一次运行里扫描多个线程数。CSV 的 `threads` 字段区分每条记录，suite 输出目录按 `threads_<count>` 拆分，terminal summary 按线程分组输出 IR 计数、整体 speedup、speedup P50/P95、async decision 和 profile 摘要，避免不同线程数的结果被混合平均。未设置该变量时仍沿用单个 `COMPILER2026_DAG_THREADS`，默认值为 `4`。成功运行后，脚本默认删除 `${LABEL}` 下的大体量 per-suite 输入/输出/profile 目录，保留 `${LABEL}.csv`、`bin/` 和 `ir/`；需要排查单次输出时设置 `COMPILER2026_BENCH_KEEP_ARTIFACTS=1`。

离线参数调优使用 `submission/scripts/tune_params.sh`，不要放进 contestant 计时路径。该包装脚本默认遍历 `COMPILER2026_TUNE_ASYNC_MIN_B_LIST=18,24,32,48` 和 `COMPILER2026_TUNE_TASK_BATCH_LIST=auto,4,8`，每个组合调用一次 `benchmark.sh`，再让 `benchmark.sh` 扫 `COMPILER2026_TUNE_THREAD_LIST` 指定的线程列表。所有组合的原始行会追加到 `build/optimization_benchmarks/<label>_aggregate.csv`，terminal 会按 `threads + async_min_b + task_batch` 输出 `tune_summary`。

真实目标机上建议先用 `REPEAT=1` 扫宽参数空间，筛掉明显退化或 `dag_missing_deps` 非零的组合；再对候选组合用 `REPEAT=3` 或更高重复次数复测。当前默认 `b >= 18` / auto batch 8 只代表本 VM 上已归档的稳妥默认，不代表不同 CPU 架构、核心数、cache/内存带宽或 NUMA 拓扑下的最终最优点。

示例：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
COMPILER2026_TUNE_THREAD_LIST=1,2,4,8 \
COMPILER2026_TUNE_ASYNC_MIN_B_LIST=18,24,32,48 \
COMPILER2026_TUNE_TASK_BATCH_LIST=auto,4,8 \
COMPILER2026_TUNE_LABEL_PREFIX=target_param_sweep \
REPEAT=1 ./submission/scripts/tune_params.sh
```

新增 CSV 字段包括 `task_batch`、`runtime_batch_avg`、`runtime_batch_max`、`async_min_b`、`async_min_blocks`、`ir_submit_deps`、`ir_submit_plain`、`ir_wait_calls`、`ir_trsm_calls`、`ir_madd_calls`、`async_decisions`、`async_enabled`、`async_disabled`、`async_disabled_small_b`、`async_disabled_small_blocks`、`async_disabled_threads`、`async_disabled_single_block`、`profile_calls`、`total_tasks`、`main_tasks`、`worker_tasks`、`ready_samples`、`ready_sum`、`ready_avg`、`ready_per_thread`、`dag_nodes`、`dag_edges`、`dag_satisfied_deps`、`dag_missing_deps`、`dag_initial_ready`、`dag_released`、`dag_release_batches`、`max_dag_release_batch`、`max_dag_pending`、`max_dag_successors`、`max_dag_live`、`queue_ms`、`exec_ms`、`worker_idle_ms`、`main_wait_ms`、`wait_calls`、`wait_ms`、`wait_ready_avg`、`wait_active_avg`、`wait_dag_live_avg`、`max_wait_ready`、`max_wait_active`、`max_wait_dag_live`、`trsm_count`、`madd_count` 等。每个 suite/run 可能包含多个矩阵调用，benchmark 会把同一次运行里的 IR 计数和 profile 行聚合到同一条 CSV 记录。默认不打开 profile 时动态 profile 字段为 `0`，计时 CSV 结构保持一致。benchmark 终端摘要同时输出 IR call site 计数、所有行的 `serial_total`、`contestant_total`、算术平均 speedup、几何平均 speedup、speedup P50/P95 和 profile 模式下的 runtime batch、ready width、DAG release batch、wait span、wait pressure 与依赖状态摘要，避免后续调参只看单 suite 或手算几何平均。

示例输出节选：

```text
[compiler2026_profile] n=1024 b=32 threads=4 workers=3 batch=8 tasks=5952 ...
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
LABEL=live_window_default_repeat3_final REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
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
docs/benchmark_results/main_wait_profile_smoke.csv
docs/benchmark_results/dag_live_profile_smoke.csv
docs/benchmark_results/dag_dep_state_smoke.csv
docs/benchmark_results/ready_width_profile_smoke.csv
docs/benchmark_results/adaptive_batch_profile_smoke.csv
docs/benchmark_results/wait_span_profile_smoke.csv
docs/benchmark_results/ir_wait_count_profile_smoke.csv
docs/benchmark_results/dag_release_batch_profile_smoke.csv
docs/benchmark_results/wait_pressure_profile_smoke.csv
docs/benchmark_results/recursive_gep_key_smoke.csv
docs/benchmark_results/smoke_env_passthrough_profile_smoke.csv
docs/benchmark_results/block_coordinate_key_smoke.csv
docs/benchmark_results/async_min_blocks_profile_smoke.csv
docs/benchmark_results/async_min_blocks5_profile_smoke.csv
docs/benchmark_results/thread_sweep_profile_smoke.csv
docs/benchmark_results/batch8_default_repeat3.csv
docs/benchmark_results/batch8_default_profile_smoke.csv
docs/benchmark_results/async_min_b24_default_repeat3.csv
docs/benchmark_results/async_min_b24_default_profile_smoke.csv
docs/benchmark_results/tune_wrapper_smoke_aggregate.csv
docs/benchmark_results/tune_wrapper_profile_smoke_aggregate.csv
docs/benchmark_results/async_min_b18_madd_no_output_default_repeat3.csv
docs/benchmark_results/async_min_b18_madd_no_output_default_profile_smoke.csv
docs/benchmark_results/cross_panel_gate_default_repeat3.csv
docs/benchmark_results/cross_panel_opt_in_profile_smoke.csv
docs/benchmark_results/successor_edge_pool_repeat3.csv
docs/benchmark_results/successor_edge_pool_profile_smoke.csv
docs/benchmark_results/benchmark_artifact_cleanup_smoke.csv
docs/benchmark_results/benchmark_percentile_summary_smoke.csv
docs/benchmark_results/live_window_default_repeat3_final.csv
docs/benchmark_results/cross_panel_live512_smoke.csv
docs/benchmark_results/cross_panel_live1024_smoke.csv
docs/benchmark_results/cross_panel_live2048_smoke.csv
docs/benchmark_results/cross_panel_live4096_smoke.csv
docs/benchmark_results/cross_panel_live2048_final_smoke.csv
docs/benchmark_results/cross_panel_live2048_profile_final_smoke.csv
```

前三个 CSV 来自早期“整函数替换为 runtime 入口”的实验版本。它们的性能更高，但该路线不够符合赛题对 IR 层算子依赖分析的要求，因此不作为当前提交方案。
`profile_csv_smoke.csv`、`ready_queue_profile_csv_smoke.csv`、`dag_profile_counters_smoke.csv`、`panel_dag_cleanup_profile_smoke.csv`、`async_predicate_profile_smoke.csv`、`async_predicate_disabled_smoke.csv`、`async_predicate_threads1_smoke.csv`、`async_decision_profile_smoke.csv`、`async_decision_threads1_smoke.csv`、`benchmark_overall_summary_smoke.csv`、`dag_successor_fanout_smoke.csv`、`gep_operator_key_smoke.csv`、`ir_submit_counts_smoke.csv`、`dag_reserve_structures_smoke.csv`、`panel_task_reserve_smoke.csv`、`queue_reset_lock_smoke.csv`、`main_wait_profile_smoke.csv`、`dag_live_profile_smoke.csv`、`dag_dep_state_smoke.csv`、`ready_width_profile_smoke.csv`、`adaptive_batch_profile_smoke.csv`、`wait_span_profile_smoke.csv`、`ir_wait_count_profile_smoke.csv`、`dag_release_batch_profile_smoke.csv`、`wait_pressure_profile_smoke.csv`、`recursive_gep_key_smoke.csv`、`smoke_env_passthrough_profile_smoke.csv`、`block_coordinate_key_smoke.csv`、`async_min_blocks_profile_smoke.csv`、`async_min_blocks5_profile_smoke.csv`、`thread_sweep_profile_smoke.csv`、`batch8_default_profile_smoke.csv`、`async_min_b24_default_profile_smoke.csv`、`tune_wrapper_profile_smoke_aggregate.csv`、`async_min_b18_madd_no_output_default_profile_smoke.csv`、`successor_edge_pool_profile_smoke.csv` 和 `cross_panel_live2048_profile_final_smoke.csv` 是 profile 数据链验证用的单次重复实验，用于确认 CSV 字段、聚合逻辑、阈值开关、最小 block 数开关、线程数开关、线程数扫参 summary 分组、离线调参 aggregate 汇总、smoke/benchmark 环境透传、async decision 原因聚合、整体 summary 输出、DAG successor fanout 统计、DAG release batch 统计、wait 入口 pressure 统计、block key 恢复 smoke 行为、block row/col 恢复到 runtime key 的路径、递归一维 GEP key 恢复路径、IR call site 计数、静态 wait call site 计数、DAG reserve 行为、panel task reserve 估算、runtime reset 加锁后的 profile 链路、main wait 空等统计、DAG live-pressure 统计、依赖解析状态统计、ready queue 宽度采样统计、自适应 runtime batch 记录、wait span 统计和 live-window drain 行为，不作为正式性能均值。`tune_wrapper_smoke_aggregate.csv` 是调参 wrapper 的单组合非 profile smoke；`ready_queue_batch8_repeat3.csv` 是早期 task batch 调参对照；`batch8_default_repeat3.csv` 是默认 batch 调整后的正式重复结果；`async_min_b24_default_repeat3.csv` 是上一版默认阈值调整后的正式重复结果；`async_min_b18_madd_no_output_default_repeat3.csv` 是上一版默认阈值和 panel-local output key 优化后的正式重复结果；`successor_edge_pool_repeat3.csv` 是上一版 runtime successor edge pool 优化后的正式重复结果；`live_window_default_repeat3_final.csv` 是当前默认路径正式重复结果。
`cross_panel_opt_in_profile_smoke.csv` 是跨 panel DAG 实验路径的 profile 记录，显示该路径把 panel 内静态 wait 降为外层 DAG 收尾 wait：`ir_wait_calls=1`、`ir_submit_deps=3`，但 4 vCPU VM 上 `speedup_geo=1.499x`、`max_dag_live=6072`、`dag_missing_deps=7595`，低于默认 panel-local 方案。`cross_panel_gate_default_repeat3.csv` 验证新增 gate 后默认路径仍保持 panel-local：`ir_wait_calls=1`、`speedup_geo=1.633x`。
`benchmark_artifact_cleanup_smoke.csv` 是 benchmark artifact cleanup 的单次 smoke 记录，用于证明 CSV 保留和默认清理路径可用，不作为正式性能均值。
`benchmark_percentile_summary_smoke.csv` 是 terminal percentile summary 的单次 smoke 记录，用于证明 suite/overall summary 输出 `speedup_p50` 和 `speedup_p95` 可用；该 CSV 不新增字段，不作为正式性能均值。
`cross_panel_live512_smoke.csv`、`cross_panel_live1024_smoke.csv`、`cross_panel_live2048_smoke.csv` 和 `cross_panel_live4096_smoke.csv` 是 live-window 候选扫参记录；最佳单次 smoke 未超过当前默认 repeat=3，因此不切默认。`cross_panel_live2048_profile_final_smoke.csv` 证明 `COMPILER2026_DAG_MAX_LIVE=2048` 下 `max_dag_live=2050`，但 `contestant_total=0.658224s`、`speedup_geo=1.550x` 仍不足以默认启用。

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
