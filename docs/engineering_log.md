# 工程经验记录

本文记录每轮优化留下的可复用经验，避免后续不同优化方向互相掣肘。只记录已经通过构建或 smoke/benchmark 验证的事实；未验证结论放回 roadmap，不写成性能事实。

## 2026-06-06 profiling 和 benchmark 数据链

改动：

- runtime 增加 `COMPILER2026_DAG_PROFILE=1` 观测模式，输出 task 数、队列等待、执行时间、worker idle、批量出队和按 task 名称聚合的 `trsm/madd` 统计。
- Pass 在 async path 入口注册 `compiler2026_task_trsm/madd` 的 profile 名称。注册只提供统计标签，不改变调度语义。
- `submission/scripts/benchmark.sh` 在 contestant 每次运行时捕获 profile stderr，并把同一 suite/run 内的多次矩阵调用 profile 聚合写入同一行 CSV。

经验：

- profile 必须默认关闭。默认评测路径不应因为观测代码引入明显额外计时、锁竞争或输出噪声。
- profile 字段必须和 timing/verifier 结果绑定在同一条 benchmark 记录里，否则后续很难判断某个阈值或 batch 策略为什么有效。
- 一个 benchmark suite 通常包含多条规格，profile parser 必须聚合所有 `[compiler2026_profile]` 行；只取最后一行会严重低估 task 数和队列等待。
- runtime 可以知道 task function 的观测名称，但不能把名称变成调度语义；调度仍只依赖函数指针和 context 指针，官方 `trsm/madd` ABI 仍保留在 Pass 生成的 task function 内。
- profile 输出适合指导 `COMPILER2026_TASK_BATCH`、async 阈值和未来 range task；它还不能证明 ready-queue DAG 已经实现。

验证：

- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过。
- `SPEC_START=93 SPEC_END=93 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/smoke_test.sh` verifier 通过，并输出 `trsm/madd` profile 行。
- `COMPILER2026_DAG_PROFILE=1 LABEL=profile_csv_smoke REPEAT=1 ./submission/scripts/benchmark.sh` 生成包含聚合 profile 字段的 CSV，归档为 `docs/benchmark_results/profile_csv_smoke.csv`。

后续：

- 把 profile CSV 字段用于自动选择 `COMPILER2026_TASK_BATCH` 和 async 阈值。
- 在恢复 block 坐标后，为 ready-queue DAG 增加依赖边数量、ready queue 深度和关键路径相关 profile。

## 2026-06-06 panel 内 ready queue DAG

改动：

- Pass 从 `trsm/madd` call operands 的直接 GEP offset 恢复 block key，使用 `offset / b` 作为同一矩阵调用内的唯一 block 标识。
- 新增 `compiler2026_runtime_submit_deps(fn, ctx, dep_a, dep_b, output)`。Pass 对 `trsm` 传输出 key，对 `madd` 传两个输入 key 和输出 key。
- runtime 增加 generic ready queue DAG：记录每个 output key 的 latest producer，依赖完成后释放 successor task。
- 移除 `trsm` loop exit 的全局 wait；保留 panel 末尾 wait，暂不跨 panel 提前启动。

经验：

- 第一版 DAG 要限制作用域。先做 panel 内 `trsm -> madd`，保留 panel 末尾 barrier，可以验证 block key 和 runtime 依赖机制，而不同时承担跨 panel 正确性风险。
- runtime 只能理解整数 key 和函数指针，不理解 `trsm/madd` 语义；算子依赖知识留在 Pass 里，避免 runtime 变成手写算法。
- block key 恢复依赖当前 O2 IR 的直接 GEP 形态。Pass 必须有 fallback：如果 key 恢复失败，回到原 submit/wait 路径。
- profile/benchmark CSV 能快速判断调度变化是否值得保留。本轮 4-vCPU VM 上几何平均 speedup 从 `1.469x` 提升到约 `1.509x`。

验证：

- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.815x`。
- 优化后 IR 包含 `compiler2026_runtime_submit_deps`，`compiler2026_task_trsm` 仍直接调用 `@trsm`，`compiler2026_task_madd` 仍直接调用 `@madd`。
- `LABEL=runtime_ready_queue_trsm_deps REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/runtime_ready_queue_trsm_deps.csv`。
- `LABEL=ready_queue_profile_csv_smoke REPEAT=1 COMPILER2026_DAG_PROFILE=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 生成 profile CSV，归档为 `docs/benchmark_results/ready_queue_profile_csv_smoke.csv`。

后续：

- 恢复 `cholesky(panel)` 和下一 panel 相关更新的 block key，尝试跨 panel ready queue。

## 2026-06-06 DAG profile counters

改动：

- runtime profile 增加 `dag_nodes`、`dag_edges`、`dag_initial_ready`、`dag_released`、`max_dag_pending`。
- benchmark CSV 解析并聚合同名字段，profile summary 额外显示平均 DAG edge 数。

经验：

- DAG 调度的性能不能只看 task 总数。`dag_edges` 和 `dag_released` 能反映依赖图是否真的释放了 ready queue 并行度，`max_dag_pending` 能快速暴露是否引入了超过当前 runtime 模型预期的入度。
- 这些 counters 仍然只在 `COMPILER2026_DAG_PROFILE=1` 下采集，默认性能路径保持低开销。
- 本轮只增加观测能力，不改变调度语义；如果后续跨 panel DAG 性能回退，可以用这组字段区分“依赖太保守”和“runtime 队列瓶颈”。

验证：

- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过。
- `LABEL=dag_profile_counters_smoke REPEAT=1 COMPILER2026_DAG_PROFILE=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 生成包含 DAG profile 字段的 CSV。

后续：

- 用 `dag_edges / dag_nodes`、`dag_released / dag_edges`、`max_dag_pending` 评估跨 panel DAG 的依赖粒度。
- 把 DAG counters 纳入后续不同线程数和大核数平台的 profile 对比。

## 2026-06-06 benchmark tuning metadata

改动：

- benchmark CSV 新增 `task_batch` 和 `async_min_b` 字段，记录 `COMPILER2026_TASK_BATCH` 与 `COMPILER2026_ASYNC_MIN_B` 的有效实验环境。
- 归档 `ready_queue_batch8_repeat3.csv` 作为 ready queue 下 task batch 对照实验。

经验：

- `COMPILER2026_TASK_BATCH=8` 在单次 smoke 中几何平均最好，3 次重复下 speedup 几何平均也略高于默认，但 contestant 总耗时没有稳定优于默认。因此本轮不改默认 heuristic。
- 调参结果必须把环境变量写入 CSV；否则后续无法区分代码变化、运行波动和环境覆盖。
- 默认策略调整需要以 contestant time、几何平均 speedup、正确性和 profile counters 一起判断，不能只看单个 suite 或单次 speedup。

验证：

- `LABEL=runtime_ready_queue_trsm_deps REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 通过，归档的 CSV 已更新为包含调参环境字段；本轮默认几何平均约 `1.520x`。
- `LABEL=ready_queue_batch8_repeat3 REPEAT=3 COMPILER2026_TASK_BATCH=8 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/ready_queue_batch8_repeat3.csv`，几何平均约 `1.525x`。
- `LABEL=dag_profile_counters_smoke REPEAT=1 COMPILER2026_DAG_PROFILE=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 通过，CSV 同时包含 `task_batch`、`async_min_b` 和 DAG counters。
