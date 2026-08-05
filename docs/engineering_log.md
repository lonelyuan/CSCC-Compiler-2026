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

## 2026-06-06 panel DAG wait cleanup

改动：

- runtime 在 `wait()` 确认 ready queue 为空、运行中任务为零、未完成 DAG 节点为零后，清理 panel-local `dag_nodes` 和 `latest_producer` 状态。
- profile 累计计数不在 `wait()` 清理，仍由 `runtime_end()` 汇报。

经验：

- panel-local DAG 的状态生命周期应该和 panel barrier 对齐。等待边界已经是正确性同步点，清理 producer 表可以避免后续 panel 误用旧 producer，也限制每轮调用中的依赖图状态增长。
- 清理调度结构和清理 profile counters 要分开；否则 profile benchmark 会因为 panel 间清理而低估 DAG 节点和边数。

验证：

- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.855x`。
- `LABEL=panel_dag_cleanup_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/panel_dag_cleanup_profile_smoke.csv`；profile 汇总仍包含 `tasks_avg=6999.2`、`dag_edges_avg=5682.8`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-06 runtime async predicate

改动：

- Pass 入口分支从硬编码 `b >= 32` 改为调用 `compiler2026_runtime_should_async(n, b)`。
- runtime predicate 默认仍要求 `b >= 32` 且 block 数大于 1，并读取 `COMPILER2026_ASYNC_MIN_B` 作为实验阈值。
- 归档默认阈值和禁用 async 的 profile smoke CSV，用于证明 `async_min_b` 字段对应真实执行路径。

经验：

- benchmark 元数据必须对应真实控制面。之前 CSV 记录 `async_min_b`，但 Pass 入口分支仍是编译期常量，容易让后续阈值实验得出假结论。
- async path 开关放在 runtime predicate 后，Pass 只负责生成两个版本和调用判定；阈值策略可以继续演进，不需要每次改 Pass IR 结构。
- 高阈值禁用 async 后 profile CSV 中 `profile_calls`、`total_tasks`、`dag_nodes` 全为 0，这是比只看时间更直接的路径验证。

验证：

- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.824x`。
- 优化后 IR 包含 `compiler2026_runtime_should_async`，`compiler2026_task_trsm` 仍直接调用 `@trsm`，`compiler2026_task_madd` 仍直接调用 `@madd`。
- `LABEL=async_predicate_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/async_predicate_profile_smoke.csv`；profile 汇总包含 `tasks_avg=6999.2`、`dag_edges_avg=5640.0`。
- `LABEL=async_predicate_disabled_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 COMPILER2026_ASYNC_MIN_B=9999 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/async_predicate_disabled_smoke.csv`；所有 suite 的 `profile_calls` 和 DAG/task counters 为 0。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-06 thread-aware async predicate

改动：

- `compiler2026_runtime_should_async(n, b)` 复用 `resolveThreadCount(n, b)`，只有解析后的可用线程数大于 1 时才进入 async path。
- `COMPILER2026_DAG_THREADS=1` 现在直接走原始串行 IR path，避免单线程 profile/对照实验承担任务化和 DAG runtime 开销。
- 归档 `async_predicate_threads1_smoke.csv`，证明线程数开关对应真实执行路径。

经验：

- async path 是否启用应该和 worker 数选择使用同一个规模/环境变量解析逻辑。否则 `COMPILER2026_DAG_THREADS=1` 这种对照实验会看起来是单线程，实际却仍在执行 task outline、arena 和 wait 逻辑。
- 对入口 predicate 的验证不能只看耗时；单线程 profile CSV 的 `profile_calls=0`、`total_tasks=0`、`dag_nodes=0` 更直接证明没有进入 async clone。
- 这个改动不触碰 DAG 依赖模型，只收紧入口决策，为后续按真实平台核心数做 profile-guided heuristic 留出控制点。

验证：

- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.836x`。
- `LABEL=async_predicate_threads1_smoke REPEAT=1 COMPILER2026_DAG_THREADS=1 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/async_predicate_threads1_smoke.csv`；所有 suite 的 `profile_calls` 和 DAG/task counters 为 0。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-06 async decision profile

改动：

- `compiler2026_runtime_should_async` 在 `COMPILER2026_DAG_PROFILE=1` 时输出 `[compiler2026_async_decision]`，记录 `n`、`b`、block 数、阈值、线程数、是否启用 async 和原因。
- benchmark CSV 新增 `async_decisions`、`async_enabled`、`async_disabled`、`async_disabled_small_b`、`async_disabled_threads`、`async_disabled_single_block` 字段，并在 summary 中打印 enabled/disabled 聚合。
- 归档默认 4 线程和 1 线程两组 decision profile smoke CSV。

经验：

- 只记录 task/DAG counters 不能解释“为什么没有 task”。async decision 行把入口 predicate 的原因显式沉淀到 CSV，后续调阈值和线程数时不需要从 0 task 反推。
- small block 和 thread count 是两类不同的禁用原因，必须分开计数；否则真实平台上判断默认阈值是否过保守会混入单线程/少线程实验噪声。
- decision profile 只在 `COMPILER2026_DAG_PROFILE=1` 下输出，默认运行路径不增加 stderr 或 CSV 解析负担。

验证：

- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.843x`。
- `LABEL=async_decision_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/async_decision_profile_smoke.csv`；summary 为 `enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，同时保留 DAG profile 聚合。
- `LABEL=async_decision_threads1_smoke REPEAT=1 COMPILER2026_DAG_THREADS=1 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/async_decision_threads1_smoke.csv`；summary 为 `enabled=0 disabled=39 small_b=20 threads=19 single_block=0`，task/DAG counters 为 0。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-06 benchmark overall summary

改动：

- benchmark 终端摘要新增整体 `runs`、`serial_total`、`contestant_total`、算术平均 speedup 和几何平均 speedup。
- 保留 suite 级均值、async decision summary 和 profile summary，CSV 字段不变。
- 归档 `benchmark_overall_summary_smoke.csv`，用于证明新 summary 与现有 profile CSV 解析兼容。

经验：

- 默认策略是否值得保留不能只看单个 suite。把总耗时和几何平均放进 benchmark 输出，可以让后续调 `COMPILER2026_TASK_BATCH`、`COMPILER2026_ASYNC_MIN_B` 或 DAG 策略时直接比较同一套判据。
- CSV 仍是事实来源，summary 只是由同一 CSV 派生的读数；这样不会引入新的结果格式或和历史 CSV 分叉。
- 单次 smoke 的整体 summary 只能证明脚本和解析逻辑，不作为正式性能均值；正式结论仍需要 `REPEAT=3` 或真实平台重复实验。

验证：

- `bash -n submission/scripts/benchmark.sh` 通过。
- `LABEL=benchmark_overall_summary_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/benchmark_overall_summary_smoke.csv`。
- 本轮 smoke 输出整体摘要：`runs=4 serial_total=0.984008s contestant_total=0.660335s speedup_avg=1.519x speedup_geo=1.493x`。
- 同一次输出仍包含 async decision summary：`enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，以及 DAG profile summary。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-06 DAG successor fanout profile

改动：

- runtime profile 新增 `max_dag_successors`，记录单个 DAG producer 节点积累的最大 successor 数。
- benchmark CSV 和 profile summary 解析 `max_dag_successors`，用于观察 `trsm` 释放多个 `madd` 时的 fanout 压力。
- 归档 `dag_successor_fanout_smoke.csv`，证明该字段从 runtime stderr 到 CSV 和 summary 全链路可用。

经验：

- `max_dag_pending` 只能说明 consumer 入度，目前通常是 2；它不能说明 producer 是否存在高 fanout。跨 panel DAG 和 work stealing 设计需要同时看入度和出度压力。
- fanout 是调度结构指标，不是性能结论。它能提示哪些 producer 可能成为 ready release 热点，但是否需要改队列结构仍要结合 queue/exec/idle 和真实多核实验。
- 该指标只在 profile 模式维护，不改变默认调度路径。

验证：

- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.854x`。
- `LABEL=dag_successor_fanout_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/dag_successor_fanout_smoke.csv`。
- 本轮 profile summary 包含 `max_dag_successors=34`，同时保留 async decision、overall 和 DAG profile summary。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-06 GEPOperator block key recovery

改动：

- Pass 的 block key 恢复从只接受 `GetElementPtrInst` 扩展为接受 strip pointer casts 后的一维 `GEPOperator`。
- 仍然只处理一维 GEP offset 并用 `offset / b` 生成 block key；更复杂地址表达式继续回退到原 submit/wait 路径。
- 归档 `gep_operator_key_smoke.csv`，确认当前公开 IR 仍生成 `submit_deps`，DAG/profile counters 正常。

经验：

- LLVM IR 中 GEP 不一定总以 instruction 形态出现。把 key 恢复提升到 `GEPOperator` 可以覆盖 constant expression 和 cast 包裹形态，同时不改变当前依赖语义。
- 这只是形态兼容性增强，不是完整 block-coordinate 分析。二维坐标、跨 panel 依赖和读写集合仍需要后续单独实现和验证。
- 保留 fallback 很重要：一旦地址表达式超出当前模型，宁可回到保守 barrier，也不能生成错误依赖边。

验证：

- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.848x`。
- 优化后 IR 包含 `compiler2026_runtime_submit_deps`，`compiler2026_task_trsm` 仍直接调用 `@trsm`，`compiler2026_task_madd` 仍直接调用 `@madd`。
- `LABEL=gep_operator_key_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/gep_operator_key_smoke.csv`；summary 包含 `tasks_avg=6999.2`、`dag_edges_avg=5866.8`、`max_dag_successors=35`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-06 benchmark IR submit counters

改动：

- benchmark 在 `opt` 后反汇编 `app.opt.bc`，统计 `ir_submit_deps`、`ir_submit_plain`、`ir_trsm_calls`、`ir_madd_calls`。
- 这些静态 IR call site 计数写入 CSV，并在 benchmark summary 中输出一次。
- 归档 `ir_submit_counts_smoke.csv`，用于证明 Pass 当前仍走 dependency-aware submit 路径。

经验：

- 动态 DAG profile 只能说明运行时实际执行了多少 task；如果 Pass key 恢复回退，最好能从同一份 CSV 直接看到 IR 里普通 submit 是否重新出现。
- 静态 IR 计数和动态 profile 是互补证据：`ir_submit_deps=2` 说明编译后保留依赖提交 call site，`dag_nodes/dag_edges` 说明运行时确实构建 DAG。
- 本轮第一次 benchmark 失败是 VM 根分区被旧 `build/optimization_benchmarks` 输出占满，baseline 写结果失败；清理生成目录后重跑通过。后续遇到 `Failed to write result payload` 应先检查 `df -h` 和 benchmark 输出目录大小。

验证：

- `bash -n submission/scripts/benchmark.sh` 通过。
- `LABEL=ir_submit_counts_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 在 VM 清理旧 benchmark 输出后通过，归档为 `docs/benchmark_results/ir_submit_counts_smoke.csv`。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 trsm_calls=2 madd_calls=2`，同时保留 overall、async decision 和 DAG profile summary。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-06 reserve DAG runtime structures

改动：

- `resetQueue` 复用已有 `reserveTaskCount(n, b)` 的容量提示，除 ready queue 外也预留 `dag_nodes` 和 `latest_producer`。
- 该改动不改变依赖语义，只降低 panel-local DAG 提交阶段 vector 扩容和 hash table rehash 的风险。
- 归档 `dag_reserve_structures_smoke.csv`，确认 IR submit 计数和 DAG profile counters 仍正常。

经验：

- ready queue 容量复用只覆盖任务就绪队列，不覆盖依赖图本身；panel 内 DAG 变大后，`dag_nodes` 和 producer map 也应该跟着预留。
- `reserveTaskCount` 是保守容量提示，不是正确性条件。即使估算不足，容器仍会增长；估算过大也只影响容量，不改变调度结果。
- 这类 runtime 结构优化需要用 correctness smoke 和 profile/IR counters 保证没有悄悄退回普通 submit 或破坏 DAG。

验证：

- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.832x`。
- `LABEL=dag_reserve_structures_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/dag_reserve_structures_smoke.csv`。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 trsm_calls=2 madd_calls=2`，profile summary 包含 `tasks_avg=6999.2`、`dag_edges_avg=5673.8`、`max_dag_successors=35`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-06 panel task reserve estimate

改动：

- `reserveTaskCount(n, b)` 的容量提示从只估算首个 panel 的 `madd` 数量，修正为 `trsm + madd` 总任务数。
- ready queue、DAG node vector 和 latest-producer hash table 继续复用同一个容量提示；本轮只修正估算值，不改变调度依赖语义。
- 归档 `panel_task_reserve_smoke.csv`，确认修正后 IR submit 计数、async decision 和 DAG profile counters 仍正常。

经验：

- panel-local DAG 的最大任务量来自首个 panel：`trsm` 为 `trailing` 个，`madd` 为 `trailing * (trailing + 1) / 2` 个。之前只按 `madd` 数预留，结构会在提交 `trsm` 后仍有额外增长风险。
- 容量预留不能写成隐含算法事实。它只是 runtime 容器提示，不能成为正确性条件，也不能把 runtime 变成算子专用实现。
- 结构型微优化仍要用 IR 计数和动态 profile 交叉检查，避免容量改动掩盖依赖提交路径退化。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.833x`。
- `LABEL=panel_task_reserve_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/panel_task_reserve_smoke.csv`。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 trsm_calls=2 madd_calls=2`，overall summary 为 `runs=4 serial_total=0.958934s contestant_total=0.642000s speedup_avg=1.518x speedup_geo=1.493x`，async decision summary 为 `enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，profile summary 包含 `tasks_avg=6999.2`、`dag_edges_avg=5595.2`、`max_dag_successors=35`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-06 runtime queue reset locking

改动：

- `AsyncRuntime::resetQueue` 在调整 ready queue、DAG node vector、latest-producer 表、pending batch 状态和 worker error 状态前持有 runtime mutex。
- 同步更新 `docs/optimization_principles.md` 中过期的两阶段 wait 描述，当前状态明确为 panel 内 ready queue DAG、panel 间 barrier。
- 归档 `queue_reset_lock_smoke.csv`，确认加锁 reset 后 IR submit 计数、async decision 和 DAG profile counters 仍正常。

经验：

- `wait()` 返回只能说明当前任务完成，不能说明 worker 线程不存在。worker 池复用时，清理队列和 DAG 容器仍应遵守同一个 mutex 保护边界。
- 生命周期修复不改变调度语义，也不能写成性能提升；它的验证重点是 verifier、无死锁、IR 依赖提交路径和 profile 链路保持正常。
- 原理文档也要跟着实现演进。继续保留“进入 `madd` 前等待全部 `trsm`”的旧说法，会误导后续跨 panel DAG 设计。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.861x`。
- `LABEL=queue_reset_lock_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/queue_reset_lock_smoke.csv`。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 trsm_calls=2 madd_calls=2`，overall summary 为 `runs=4 serial_total=0.968461s contestant_total=0.647668s speedup_avg=1.515x speedup_geo=1.484x`，async decision summary 为 `enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，profile summary 包含 `tasks_avg=6999.2`、`dag_edges_avg=5107.5`、`max_dag_successors=35`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-06 main wait profile counter

改动：

- runtime profile 新增 `main_wait_ms`，记录主线程在 `compiler2026_runtime_wait()` 中没有 ready task 可执行、只能等待 worker 完成或释放依赖的时间。
- benchmark CSV 新增 `main_wait_ms` 字段，并在 profile summary 中输出 `main_wait_ms_avg`。
- 归档 `main_wait_profile_smoke.csv`，确认该字段从 runtime stderr 到 CSV 和 summary 全链路可用。

经验：

- `worker_idle_ms` 只能说明 worker 是否缺活；它不能解释主线程在 panel barrier 或依赖释放不足时是否处于空等。`main_wait_ms` 可以作为后续跨 panel DAG、range task 和队列策略的关键路径症状指标。
- 该指标只在 `COMPILER2026_DAG_PROFILE=1` 下采集，默认运行路径不增加计时调用。
- 单次 VM profile 只能证明数据链路，不应把 `main_wait_ms` 的具体数值写成性能结论；后续要和不同线程数、block size 以及真实平台结果一起比较。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.833x`。
- `LABEL=main_wait_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/main_wait_profile_smoke.csv`。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 trsm_calls=2 madd_calls=2`，overall summary 为 `runs=4 serial_total=0.972876s contestant_total=0.648549s speedup_avg=1.567x speedup_geo=1.546x`，async decision summary 为 `enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，profile summary 包含 `tasks_avg=6999.2`、`dag_edges_avg=5650.2`、`max_dag_successors=35`、`main_wait_ms_avg=1.481`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-07 DAG live-pressure profile

改动：

- runtime profile 新增 `max_dag_live`，记录一次 `block_cholesky` 调用中未完成 DAG 节点数的峰值。
- benchmark CSV 新增 `max_dag_live` 字段，并在 profile summary 中输出该字段的最大值。
- 归档 `dag_live_profile_smoke.csv`，确认该字段从 runtime stderr 到 CSV 和 summary 全链路可用。

经验：

- `dag_nodes` 是总提交量，`max_dag_live` 是同一时刻未完成 DAG 节点压力。后续跨 panel DAG 如果放宽 panel barrier，live 节点峰值会比 panel-local 模型更关键。
- `max_dag_live` 只描述调度结构压力，不是性能结论。需要和 `queue_ms`、`main_wait_ms`、`worker_idle_ms`、fanout 和 verifier 一起判断是否值得扩大 DAG 作用域。
- 该指标只在 `COMPILER2026_DAG_PROFILE=1` 下维护，默认运行路径不增加统计更新。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.892x`。
- `LABEL=dag_live_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/dag_live_profile_smoke.csv`。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 trsm_calls=2 madd_calls=2`，overall summary 为 `runs=4 serial_total=0.957114s contestant_total=0.643849s speedup_avg=1.523x speedup_geo=1.498x`，async decision summary 为 `enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，profile summary 包含 `tasks_avg=6999.2`、`dag_edges_avg=4929.0`、`max_dag_successors=35`、`max_dag_live=472`、`main_wait_ms_avg=1.613`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-07 DAG dependency state profile

改动：

- runtime profile 新增 `dag_satisfied_deps` 和 `dag_missing_deps`，分别记录提交 consumer 时 producer 已完成的依赖，以及没有找到 latest producer 的依赖。
- `dag_edges` 保持原语义：只统计提交时仍未完成、需要挂 successor 的 pending dependency edge。
- benchmark CSV 新增两列，并在 profile summary 中输出平均已满足依赖和缺失依赖总数。

经验：

- 只看 `dag_edges` 会低估 Pass 恢复出的逻辑依赖数量，因为部分 producer 可能在 consumer 提交前已经完成。`dag_satisfied_deps` 能补上这部分信息。
- `dag_missing_deps=0` 是一个有用 smoke 信号：说明当前公开 IR 下 `madd` 的两个输入 key 都能在 latest-producer 表中找到对应 `trsm` producer。它不能证明完整 block-coordinate 分析已经完成，但能快速发现 key 恢复或 producer 生命周期回退。
- 这组计数只解释依赖状态，不改变调度策略；跨 panel DAG 前应继续保留 correctness smoke 和 IR submit 计数交叉验证。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.850x`。
- `LABEL=dag_dep_state_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/dag_dep_state_smoke.csv`。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 trsm_calls=2 madd_calls=2`，overall summary 为 `runs=4 serial_total=0.970238s contestant_total=0.638944s speedup_avg=1.549x speedup_geo=1.518x`，async decision summary 为 `enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，profile summary 包含 `tasks_avg=6999.2`、`dag_edges_avg=5562.2`、`dag_satisfied_deps_avg=6397.8`、`dag_missing_deps=0`、`max_dag_live=544`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-07 ready width profile

改动：

- runtime profile 新增 `ready_samples` 和 `ready_sum`，在 ready queue 入队或批量 flush 后记录一次当前 ready 宽度。
- benchmark CSV 新增 `ready_samples`、`ready_sum`、`ready_avg` 和 `ready_per_thread`，其中 `ready_avg = ready_sum / ready_samples`，`ready_per_thread = ready_avg / threads`。
- profile summary 新增 `ready_avg_avg` 和 `ready_per_thread_avg`，用于观察 ready queue 平均宽度是否足以覆盖配置线程数。

经验：

- `max_ready` 只能说明瞬时峰值，不能说明 ready queue 是否长期有足够宽度。`ready_avg` 更适合和 `worker_idle_ms`、`main_wait_ms` 一起判断是任务过细、依赖释放不足，还是队列/批量策略本身成为瓶颈。
- `ready_per_thread` 是调参辅助指标，不是性能结论。它需要和 verifier、实际时间、DAG fanout/live、线程数和 block size 一起看。
- 本轮只改 profile 链路，不改变 ready queue 调度语义，也不改变 Pass 生成的依赖边。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.644x`。
- `LABEL=ready_width_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/ready_width_profile_smoke.csv`。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 trsm_calls=2 madd_calls=2`，overall summary 为 `runs=4 serial_total=0.949592s contestant_total=0.643393s speedup_avg=1.510x speedup_geo=1.479x`，async decision summary 为 `enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，profile summary 包含 `tasks_avg=6999.2`、`ready_avg_avg=73.825`、`ready_per_thread_avg=18.456`、`dag_edges_avg=5308.8`、`dag_satisfied_deps_avg=6651.2`、`dag_missing_deps=0`、`max_dag_live=525`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-07 adaptive runtime batch

改动：

- 默认 `selectTaskBatchSize` 不再只看 `b`，同时参考 `block_count = n / b` 和参与线程数。小 block 仍保留较大批量上限，但当 panel block 数相对线程数较少时自动收窄到 `1/2/4`，避免小 panel 下过度批量出队。
- `COMPILER2026_TASK_BATCH` 显式环境覆盖保持优先级不变，仍用于真实平台调参。
- benchmark CSV 新增 `runtime_batch_avg` 和 `runtime_batch_max`，从 runtime profile 的 `batch=` 字段解析 auto 模式实际生效的批量。

经验：

- `task_batch=auto` 只是实验配置标签，不足以证明 runtime 选了什么批量；必须把 profile 中的实际 `batch` 汇总进 CSV。
- 只按 `b` 选择 batch 会忽略 panel 宽度。`block_count` 接近线程数时，大批量出队容易降低公平性；block 数足够大时，保留批量仍能减少全局队列锁开销。
- 这是默认调度启发式变化，不改变 Pass 依赖恢复，也不改变 task function 对官方 `trsm/madd` ABI 的直接调用。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.829x`。
- `LABEL=adaptive_batch_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/adaptive_batch_profile_smoke.csv`。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 trsm_calls=2 madd_calls=2`，overall summary 为 `runs=4 serial_total=0.962757s contestant_total=0.646104s speedup_avg=1.514x speedup_geo=1.486x`，async decision summary 为 `enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，profile summary 包含 `tasks_avg=6999.2`、`runtime_batch_max=16`、`ready_avg_avg=82.026`、`ready_per_thread_avg=20.506`、`dag_edges_avg=4847.8`、`dag_satisfied_deps_avg=7112.2`、`dag_missing_deps=0`、`max_dag_live=504`。
- CSV 显示本轮 auto batch 已按 suite 收窄：`n512_576 runtime_batch_max=4`、`n768 runtime_batch_max=4`、`n1024 runtime_batch_max=4`、`n1152_small_b runtime_batch_max=16`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-07 wait span profile

改动：

- runtime profile 新增 `wait_calls` 和 `wait_ms`，记录 profile 期间 `compiler2026_runtime_wait()` 的调用次数和总 wall time。
- benchmark CSV 新增 `wait_calls` 和 `wait_ms` 字段，并在 profile summary 中输出 `wait_ms_avg`。
- 归档 `wait_span_profile_smoke.csv`，确认该字段从 runtime stderr 到 CSV 和 summary 全链路可用。

经验：

- `main_wait_ms` 只记录主线程在 wait 中没有 ready task 可执行时的空等时间；`wait_ms` 记录整个 wait 区间，可以更直接量化 panel barrier 和最终 `runtime_end()` wait 的调度轮廓。
- `wait_calls` 能帮助区分“少量长 wait”和“很多短 wait”。后续替换 panel 末尾 barrier 或引入跨 panel DAG 时，应同时看 `wait_calls`、`wait_ms`、`main_wait_ms`、`worker_idle_ms` 和 verifier。
- 本轮只增加 profile 观测，不改变 wait 语义，也不减少 panel 间 barrier。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.810x`。
- `LABEL=wait_span_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/wait_span_profile_smoke.csv`。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 trsm_calls=2 madd_calls=2`，overall summary 为 `runs=4 serial_total=0.967136s contestant_total=0.640435s speedup_avg=1.532x speedup_geo=1.508x`，async decision summary 为 `enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，profile summary 包含 `tasks_avg=6999.2`、`runtime_batch_max=16`、`ready_avg_avg=81.403`、`ready_per_thread_avg=20.351`、`dag_edges_avg=4647.8`、`dag_satisfied_deps_avg=7312.2`、`dag_missing_deps=0`、`max_dag_live=486`、`main_wait_ms_avg=1.457`、`wait_ms_avg=45.063`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-07 static IR wait count

改动：

- benchmark 在反汇编 `app.opt.bc` 后新增 `ir_wait_calls` 静态计数，统计优化后 IR 中 `compiler2026_runtime_wait()` call site 数。
- CSV 新增 `ir_wait_calls` 字段，benchmark summary 的 `ir:` 行同步输出该计数。
- 归档 `ir_wait_count_profile_smoke.csv`，把静态 wait 插入点和动态 `wait_calls` / `wait_ms` 放在同一条记录里。

经验：

- `wait_calls` 是动态执行次数，`wait_ms` 是动态耗时；它们不能说明 Pass 静态插入了多少个 wait call site。`ir_wait_calls` 能直接证明当前 async IR 仍保留 panel barrier 相关 wait 插入点。
- 跨 panel DAG 改造时，应该同时观察 `ir_wait_calls` 是否下降、动态 `wait_calls/wait_ms` 是否下降，以及 verifier 是否仍通过。
- 静态 IR 计数只是编译产物证据，不是性能结论；仍需和 CSV 时间、profile summary、IR `submit_deps` 计数交叉判断。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.825x`。
- `LABEL=ir_wait_count_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/ir_wait_count_profile_smoke.csv`。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 wait_calls=1 trsm_calls=2 madd_calls=2`，overall summary 为 `runs=4 serial_total=0.954983s contestant_total=0.644285s speedup_avg=1.510x speedup_geo=1.486x`，async decision summary 为 `enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，profile summary 包含 `tasks_avg=6999.2`、`runtime_batch_max=16`、`ready_avg_avg=80.403`、`ready_per_thread_avg=20.101`、`dag_edges_avg=4652.2`、`dag_satisfied_deps_avg=7307.8`、`dag_missing_deps=0`、`max_dag_live=504`、`main_wait_ms_avg=2.707`、`wait_ms_avg=45.206`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-07 DAG release batch profile

改动：

- `completeDagTasksLocked` 改为返回本次完成批量释放出的 ready successor 数，调度语义仍保持 release 后 `notify_all`。
- runtime profile 新增 `dag_release_batches` 和 `max_dag_release_batch`，分别记录释放过 successor 的完成批次数，以及单次完成批量释放 successor 的峰值。
- benchmark CSV 和 profile summary 新增这两个字段，归档 `dag_release_batch_profile_smoke.csv`。

经验：

- 先试过把单个 successor release 改为 `notify_one`，但单次 profile benchmark 出现 `speedup_geo=1.451x`、`queue_ms/wait_ms` 偏高；该调度行为未保留。
- `dag_released` 是释放出的 task 总数，不能说明这些释放是集中发生还是零散发生。`dag_release_batches` 和 `max_dag_release_batch` 能给后续唤醒策略、per-worker queue 和 work stealing 设计提供更直接的证据。
- 本轮最终只增加 profile 数据并保持原 `notify_all` release 语义，避免在证据不足时改变调度策略。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.823x`。
- `LABEL=dag_release_batch_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/dag_release_batch_profile_smoke.csv`。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 wait_calls=1 trsm_calls=2 madd_calls=2`，overall summary 为 `runs=4 serial_total=0.972092s contestant_total=0.645926s speedup_avg=1.529x speedup_geo=1.504x`，async decision summary 为 `enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，profile summary 包含 `tasks_avg=6999.2`、`runtime_batch_max=16`、`ready_avg_avg=81.272`、`ready_per_thread_avg=20.318`、`dag_edges_avg=4642.8`、`dag_satisfied_deps_avg=7317.2`、`dag_missing_deps=0`、`dag_release_batches_avg=305.0`、`max_dag_release_batch=98`、`max_dag_live=511`、`main_wait_ms_avg=1.273`、`wait_ms_avg=45.306`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-07 wait pressure profile

改动：

- runtime 在每次 `wait()` 入口记录 ready task 数、active task 数和当前 DAG live task 数的 sum/max。
- benchmark CSV 新增 `wait_ready_avg`、`wait_active_avg`、`wait_dag_live_avg`、`max_wait_ready`、`max_wait_active`、`max_wait_dag_live`，profile summary 同步输出 wait pressure 摘要。
- 归档 `wait_pressure_profile_smoke.csv`，用于观察当前 panel barrier 入口仍有多少可执行或未完成工作。

经验：

- `wait_ms` 只说明等待耗时，不说明等待入口还压着多少 ready/active/DAG 节点；跨 panel DAG 改造前需要同时看 `wait_dag_live_avg` 和 `max_wait_dag_live`。
- 本轮 profile benchmark 显示 `wait_ready_avg=66.716`、`wait_active_avg=8.046`、`wait_dag_live_avg=88.303`、`max_wait_dag_live=486`，说明 panel wait 入口仍经常面对一批未完成 DAG 工作，后续移除 barrier 有明确观测目标。
- 本轮只增加 profile 数据，不改变 `notify_all`、ready queue、DAG 依赖或 Pass 插桩语义。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.828x`。
- `LABEL=wait_pressure_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/wait_pressure_profile_smoke.csv`。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 wait_calls=1 trsm_calls=2 madd_calls=2`，overall summary 为 `runs=4 serial_total=0.962240s contestant_total=0.643975s speedup_avg=1.522x speedup_geo=1.496x`，async decision summary 为 `enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，profile summary 包含 `tasks_avg=6999.2`、`runtime_batch_max=16`、`ready_avg_avg=80.880`、`ready_per_thread_avg=20.220`、`dag_edges_avg=4648.5`、`dag_satisfied_deps_avg=7311.5`、`dag_missing_deps=0`、`dag_release_batches_avg=300.0`、`max_dag_release_batch=78`、`max_dag_live=486`、`main_wait_ms_avg=1.024`、`wait_ms_avg=44.950`、`wait_ready_avg=66.716`、`wait_active_avg=8.046`、`wait_dag_live_avg=88.303`、`max_wait_dag_live=486`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-07 recursive GEP key recovery

改动：

- Pass 的 block key 恢复拆出 `buildLinearElementOffset`，在 strip pointer casts 后递归累加嵌套一维 `GEPOperator` 的 element offset。
- `buildBlockKey` 统一把 offset 转到 pointer-size integer 后再计算 `offset / b`，直接一维 GEP 的语义保持不变。
- 多维或无法线性化的地址表达式仍然返回失败，沿用原来的普通 submit/wait fallback。

经验：

- 公开 baseline 的 O2 IR 当前仍是直接一维 GEP，因此本轮不是性能调参；价值在于降低后续 loop/GEP canonicalization 变化导致依赖提交退化的风险。
- 递归累加只覆盖同一线性 buffer 上的 GEP 链，不假装完成二维坐标或读写集合分析。跨 panel DAG 仍需要单独恢复 `row/panel` 坐标并重新验证正确性。
- IR smoke 仍显示 `submit_deps=2`、`submit_plain=0`、`wait_calls=1`，说明公开路径没有因重构退化到普通 submit。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.817x`。
- `LABEL=recursive_gep_key_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/recursive_gep_key_smoke.csv`。
- 优化后 IR 检查确认 `compiler2026_task_trsm` 直接调用 `@trsm`、`compiler2026_task_madd` 直接调用 `@madd`，async path 保持两个 `compiler2026_runtime_submit_deps` call site。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 wait_calls=1 trsm_calls=2 madd_calls=2`，overall summary 为 `runs=4 serial_total=0.958378s contestant_total=0.648671s speedup_avg=1.509x speedup_geo=1.481x`，async decision summary 为 `enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，profile summary 包含 `tasks_avg=6999.2`、`runtime_batch_max=16`、`ready_avg_avg=81.234`、`ready_per_thread_avg=20.308`、`dag_edges_avg=4530.2`、`dag_satisfied_deps_avg=7429.8`、`dag_missing_deps=0`、`dag_release_batches_avg=295.2`、`max_dag_release_batch=108`、`max_dag_live=504`、`wait_dag_live_avg=88.756`、`max_wait_dag_live=487`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-07 smoke runtime environment passthrough

改动：

- `smoke_test.sh` 现在向 contestant 透传 `COMPILER2026_DAG_PROFILE`、`COMPILER2026_TASK_BATCH` 和 `COMPILER2026_ASYNC_MIN_B`，与原有 `COMPILER2026_DAG_THREADS` 保持同一套 runtime knob。
- smoke 与 benchmark 的调参入口保持一致，小范围 verifier run 可以直接验证 async 阈值、profile 输出和 batch 覆盖是否真实生效。
- 归档 `smoke_env_passthrough_profile_smoke.csv`，确认常规 benchmark 路径未受脚本改动影响。

经验：

- 本轮前 smoke 只透传线程数；用 `COMPILER2026_ASYNC_MIN_B=9999` 做小 case 对照时，contestant 仍使用默认阈值，容易误判调参结果。
- 修复后 `SPEC_START=80 SPEC_END=80 COMPILER2026_DAG_PROFILE=1 COMPILER2026_ASYNC_MIN_B=9999 ./submission/scripts/smoke_test.sh` 输出 `threshold=9999 enabled=0 reason=small_b`；默认阈值同一 case 输出 `threshold=32 enabled=1 reason=enabled` 并产生 runtime profile。
- 这是验证基础设施修复，不改变 runtime 调度策略或 Pass IR 插桩。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `SPEC_START=80 SPEC_END=80 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 COMPILER2026_ASYNC_MIN_B=9999 ./submission/scripts/smoke_test.sh` verifier 通过，并确认 async 被阈值禁用。
- `SPEC_START=80 SPEC_END=80 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/smoke_test.sh` verifier 通过，并确认默认 async path/profile 生效。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.842x`。
- `LABEL=smoke_env_passthrough_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/smoke_env_passthrough_profile_smoke.csv`。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 wait_calls=1 trsm_calls=2 madd_calls=2`，overall summary 为 `runs=4 serial_total=0.962081s contestant_total=0.644859s speedup_avg=1.519x speedup_geo=1.491x`，async decision summary 为 `enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，profile summary 包含 `tasks_avg=6999.2`、`runtime_batch_max=16`、`ready_avg_avg=80.607`、`ready_per_thread_avg=20.152`、`dag_edges_avg=4662.0`、`dag_satisfied_deps_avg=7298.0`、`dag_missing_deps=0`、`dag_release_batches_avg=298.2`、`max_dag_release_batch=89`、`max_dag_live=500`、`wait_dag_live_avg=88.015`、`max_wait_dag_live=500`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-07 block-coordinate key recovery

改动：

- Pass 的 key 恢复从直接 `offset / b` 改为先恢复 `block_row` 和 `block_col`：`element_row = offset / n`，`element_col = offset % n`，再分别除以 `b`。
- 现有 runtime API 不变，Pass 将 `block_row * (n / b) + block_col` 重新组合为当前一维 dependency key。
- 递归一维 GEP offset 累加逻辑保留，多维或无法线性化的地址表达式仍然走普通 submit/wait fallback。

经验：

- 这一步让 pass 内部已经显式具备 row/col 坐标，为后续表达 `cholesky(panel+1)` 和 `trsm(row,panel+1)` 的跨 panel 依赖做准备，同时不改变 runtime 的通用 key 接口。
- 公开 profile benchmark 仍显示 `dag_missing_deps=0`，说明坐标化 key 与当前 latest-producer DAG 匹配。
- 本轮不是性能优化；速度波动只作为回归信号，不能解读为调度收益。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.835x`。
- `LABEL=block_coordinate_key_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/block_coordinate_key_smoke.csv`。
- 优化后 IR 检查确认 `compiler2026_task_trsm` 直接调用 `@trsm`、`compiler2026_task_madd` 直接调用 `@madd`，async path 保持两个 `compiler2026_runtime_submit_deps` call site。
- 本轮 summary 包含 `ir: submit_deps=2 submit_plain=0 wait_calls=1 trsm_calls=2 madd_calls=2`，overall summary 为 `runs=4 serial_total=0.956868s contestant_total=0.647151s speedup_avg=1.509x speedup_geo=1.485x`，async decision summary 为 `enabled=19 disabled=20 small_b=20 threads=0 single_block=0`，profile summary 包含 `tasks_avg=6999.2`、`runtime_batch_max=16`、`ready_avg_avg=82.514`、`ready_per_thread_avg=20.629`、`dag_edges_avg=4987.0`、`dag_satisfied_deps_avg=6973.0`、`dag_missing_deps=0`、`dag_release_batches_avg=301.5`、`max_dag_release_batch=147`、`max_dag_live=487`、`wait_dag_live_avg=90.232`、`max_wait_dag_live=484`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-07 async min block-count gate

改动：

- runtime predicate 新增 `COMPILER2026_ASYNC_MIN_BLOCKS`，默认值为 `2`，因此默认行为保持和上一轮一致。
- async decision profile 新增 `min_blocks` 输出，并新增禁用原因 `small_blocks`。
- benchmark 现在透传 `COMPILER2026_TASK_BATCH`、`COMPILER2026_ASYNC_MIN_B`、`COMPILER2026_ASYNC_MIN_BLOCKS` 给 contestant；CSV 新增 `async_min_blocks` 和 `async_disabled_small_blocks` 字段，summary 输出 `small_blocks`。
- smoke 也透传 `COMPILER2026_ASYNC_MIN_BLOCKS`，可用小范围 verifier 直接验证该 gate。

经验：

- 之前 benchmark 记录了 `task_batch` / `async_min_b`，但没有把这两个环境变量传给 contestant，容易让调参 CSV 变成“元数据正确、执行路径错误”。本轮一并修正 benchmark 透传。
- 默认 `min_blocks=2` 下 profile benchmark 决策分布仍为 `enabled=19 disabled=20 small_b=20 small_blocks=0 threads=0 single_block=0`，说明默认路径未改变。
- `COMPILER2026_ASYNC_MIN_BLOCKS=5` 时 profile benchmark 输出 `enabled=14 disabled=25 small_b=20 small_blocks=5 threads=0 single_block=0`，证明新 gate 在 benchmark 中真实生效；该实验几何平均降到 `1.193x`，所以不把默认值提高到 5。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `SPEC_START=80 SPEC_END=80 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 COMPILER2026_ASYNC_MIN_BLOCKS=5 ./submission/scripts/smoke_test.sh` verifier 通过，并输出 `min_blocks=5 enabled=0 reason=small_blocks`。
- `SPEC_START=80 SPEC_END=80 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/smoke_test.sh` verifier 通过，并输出 `min_blocks=2 enabled=1 reason=enabled`。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.833x`。
- `LABEL=async_min_blocks_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/async_min_blocks_profile_smoke.csv`。
- `LABEL=async_min_blocks5_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 COMPILER2026_ASYNC_MIN_BLOCKS=5 ./submission/scripts/benchmark.sh` 通过，归档为 `docs/benchmark_results/async_min_blocks5_profile_smoke.csv`。
- 默认 summary 为 `runs=4 serial_total=1.076040s contestant_total=0.665944s speedup_avg=1.644x speedup_geo=1.589x`，`async_decisions: enabled=19 disabled=20 small_b=20 small_blocks=0 threads=0 single_block=0`。
- `min_blocks=5` summary 为 `runs=4 serial_total=0.964741s contestant_total=0.761458s speedup_avg=1.222x speedup_geo=1.193x`，`async_decisions: enabled=14 disabled=25 small_b=20 small_blocks=5 threads=0 single_block=0`。
- 优化后 IR 检查确认 `compiler2026_task_trsm` 直接调用 `@trsm`、`compiler2026_task_madd` 直接调用 `@madd`，async path 保持两个 `compiler2026_runtime_submit_deps` call site。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-08 benchmark thread sweep

改动：

- `benchmark.sh` 新增 `COMPILER2026_DAG_THREAD_LIST`，支持 `1,2,4` 或空格分隔的多线程扫参；未设置时仍使用原来的 `COMPILER2026_DAG_THREADS`，默认值保持 `4`。
- 多线程扫参时 suite 输出目录拆分为 `threads_<count>/<suite>`，避免不同线程数的 `contestant_*.profile`、time 和 verifier 文件互相覆盖。
- benchmark terminal summary 改为按 `threads` 分组输出 suite 均值、IR 计数、整体 speedup、async decision 和 profile 摘要，避免把不同线程数的结果混合成一个均值。
- 同步更新 `README`、设计文档、性能记录、路线图和优化原理说明，把该功能定位为真实多核平台上的 profile-guided 实验入口。

经验：

- 本轮不改变 Pass 或 runtime 调度语义，只补齐路线图中“覆盖多线程维度”的实验基础设施。
- `threads=1` profile smoke 显示 `enabled=0 disabled=39 ... threads_disabled=19`，证明线程数扫参中单线程组仍由 runtime predicate 禁用 async path，而不是执行任务化开销。
- `threads=4` 同一 CSV 中仍保留 `submit_deps=2 submit_plain=0 wait_calls=1` 和 DAG profile counters，说明新脚本结构没有破坏 IR/profile 数据链。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `LABEL=thread_sweep_profile_smoke REPEAT=1 COMPILER2026_DAG_THREAD_LIST=1,4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/thread_sweep_profile_smoke.csv`。
- 本轮 thread sweep summary 中 `threads=1` 为 `runs=4 serial_total=0.971066s contestant_total=0.958938s speedup_avg=1.010x speedup_geo=1.010x`，async decision 为 `enabled=0 disabled=39 small_b=20 small_blocks=0 threads_disabled=19 single_block=0`。
- `threads=4` 为 `runs=4 serial_total=0.977226s contestant_total=0.685154s speedup_avg=1.453x speedup_geo=1.412x`，async decision 为 `enabled=19 disabled=20 small_b=20 small_blocks=0 threads_disabled=0 single_block=0`，profile summary 包含 `tasks_avg=6999.2`、`runtime_batch_max=16`、`ready_per_thread_avg=20.368`、`dag_missing_deps=0`、`wait_dag_live_avg=89.156`、`max_wait_dag_live=496`。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.815x`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-08 default task batch 8

改动：

- `selectTaskBatchSize` 将 `b <= 32` 的默认批量上限从 `16` 收窄到 `8`；`b <= 64` 原本也是 `8`，因此当前小/中等 block 的默认上限统一为 `8`。
- `COMPILER2026_TASK_BATCH` 覆盖能力保留，真实多核平台仍可继续调参。
- 同步更新设计、性能、路线图、原理文档和 README，并归档 `batch8_default_repeat3.csv` / `batch8_default_profile_smoke.csv`。

经验：

- 现有 profile/benchmark 基础设施已经足够支撑这类默认策略调整：可以同时检查 repeat=3 时间、IR call site、runtime batch 实际值和 verifier，不需要继续增加 profile 字段。
- 当前 VM 上重新对比：默认旧策略 repeat=3 为 `speedup_geo=1.478x`、contestant total `1.990378s`；`COMPILER2026_TASK_BATCH=8` 为 `speedup_geo=1.512x`、contestant total `1.971658s`；`COMPILER2026_TASK_BATCH=4` 为 `speedup_geo=1.497x`、contestant total `2.005406s`。
- 收窄到 `8` 后默认 repeat=3 为 `speedup_geo=1.515x`、contestant total `2.002821s`。该数值与不同 run 的 serial 波动有关，保留 total 和 geo 两个判据；结论是 batch 8 至少优于本轮旧默认和 batch 4，不再把 `16` 作为默认。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `LABEL=batch_default_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在改动前 VM 通过，旧默认 summary 为 `runs=12 serial_total=2.927189s contestant_total=1.990378s speedup_avg=1.507x speedup_geo=1.478x`。
- `LABEL=batch8_repeat3_current REPEAT=3 COMPILER2026_DAG_THREADS=4 COMPILER2026_TASK_BATCH=8 ./submission/scripts/benchmark.sh` 在改动前 VM 通过，summary 为 `runs=12 serial_total=2.976853s contestant_total=1.971658s speedup_avg=1.539x speedup_geo=1.512x`。
- `LABEL=batch4_repeat3_current REPEAT=3 COMPILER2026_DAG_THREADS=4 COMPILER2026_TASK_BATCH=4 ./submission/scripts/benchmark.sh` 在改动前 VM 通过，summary 为 `runs=12 serial_total=2.982709s contestant_total=2.005406s speedup_avg=1.526x speedup_geo=1.497x`。
- `LABEL=batch8_default_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在改动后 VM 通过，归档为 `docs/benchmark_results/batch8_default_repeat3.csv`；summary 为 `runs=12 serial_total=3.026025s contestant_total=2.002821s speedup_avg=1.547x speedup_geo=1.515x`，IR 计数仍为 `submit_deps=2 submit_plain=0 wait_calls=1 trsm_calls=2 madd_calls=2`。
- `LABEL=batch8_default_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/batch8_default_profile_smoke.csv`；summary 包含 `runtime_batch_max=8`、`dag_missing_deps=0`、`wait_dag_live_avg=89.219`。
- `SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.831x`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-08 async threshold 24

改动：

- runtime 默认 `COMPILER2026_ASYNC_MIN_B` 从 `32` 降到 `24`。
- `smoke_test.sh` 和 `benchmark.sh` 的默认元数据同步改为 `24`，避免 CSV 记录和真实 runtime 默认不一致。
- 同步更新设计、性能、路线图、原理文档和 README，并归档 `async_min_b24_default_repeat3.csv` / `async_min_b24_default_profile_smoke.csv`。

经验：

- profile 基础设施已经足够支撑阈值决策：本轮直接用 verifier、repeat=3 benchmark、async decision summary、IR call site 计数和 DAG missing deps 判断，不再新增观测字段。
- 提高阈值到 `48` 明显变差：`speedup_geo=1.315x`，说明不能简单通过禁用更多 async 来优化小 case。
- `b >= 24` 在 `SPEC_START=91 SPEC_END=104` smoke 中 verifier 通过，并在公开 benchmark repeat=3 中优于 `b >= 32`。历史 `b >= 16` 触发过段错误，因此本轮只把默认降到 `24`，不继续冒进。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `LABEL=async_min_b48_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 COMPILER2026_ASYNC_MIN_B=48 ./submission/scripts/benchmark.sh` 在 VM 通过但性能变差，summary 为 `runs=12 serial_total=2.990524s contestant_total=2.302986s speedup_avg=1.351x speedup_geo=1.315x`。
- `SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 COMPILER2026_ASYNC_MIN_B=24 ./submission/scripts/smoke_test.sh` verifier 通过。
- `LABEL=async_min_b24_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 COMPILER2026_ASYNC_MIN_B=24 ./submission/scripts/benchmark.sh` 在 VM 通过，summary 为 `runs=12 serial_total=2.989344s contestant_total=1.894840s speedup_avg=1.606x speedup_geo=1.579x`。
- `LABEL=async_min_b24_default_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在默认阈值改动后 VM 通过，归档为 `docs/benchmark_results/async_min_b24_default_repeat3.csv`；summary 为 `runs=12 serial_total=2.968690s contestant_total=1.896939s speedup_avg=1.585x speedup_geo=1.559x`，IR 计数仍为 `submit_deps=2 submit_plain=0 wait_calls=1 trsm_calls=2 madd_calls=2`。
- `LABEL=async_min_b24_default_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/async_min_b24_default_profile_smoke.csv`；async decision 为 `enabled=22 disabled=17 small_b=17 small_blocks=0 threads_disabled=0 single_block=0`，profile summary 包含 `runtime_batch_max=8`、`dag_missing_deps=0`。
- `SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在默认阈值下 verifier 通过，speedup `1.513x`。
- `./submission/scripts/package.sh` 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-08 offline parameter sweep wrapper

改动：

- 新增 `submission/scripts/tune_params.sh`，在 `benchmark.sh` 外层遍历 `COMPILER2026_TUNE_ASYNC_MIN_B_LIST` 和 `COMPILER2026_TUNE_TASK_BATCH_LIST`，每个组合再用 `COMPILER2026_TUNE_THREAD_LIST` 交给 benchmark 做线程扫参。
- 每个组合保留独立 benchmark CSV，并把所有原始行追加到 `build/optimization_benchmarks/<label>_aggregate.csv`；脚本结尾按 `threads + async_min_b + task_batch` 输出 `tune_summary`。
- 增加 `COMPILER2026_TUNE_DRY_RUN=1`，用于快速检查参数展开和 label 生成，不触发 build/benchmark。
- 同步更新 README、设计、性能、路线图和优化原理文档，把调参策略明确为“事前离线搜索，运行时廉价 deterministic 决策”。

经验：

- `COMPILER2026_ASYNC_MIN_B`、`COMPILER2026_TASK_BATCH` 和线程数都依赖 CPU 核心数、cache、内存带宽、NUMA 和调度开销；当前 4 vCPU VM 的默认值不能直接视为真实鲲鹏机器最优。
- profile 基础设施已经足够支撑离线 sweep，暂时不需要继续增加计数字段；更重要的是把已有字段按参数组合沉淀为可比较的 aggregate CSV。
- 不把在线计时搜索放进 contestant 执行路径，避免调优开销污染最终计时，也避免对单次 judge 输入做不可控自适应。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/tune_params.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- 本地 dry-run 通过：`COMPILER2026_TUNE_DRY_RUN=1 COMPILER2026_TUNE_THREAD_LIST=1,4 COMPILER2026_TUNE_ASYNC_MIN_B_LIST=18,24 COMPILER2026_TUNE_TASK_BATCH_LIST=auto,8 COMPILER2026_TUNE_LABEL_PREFIX='dry run' REPEAT=1 ./submission/scripts/tune_params.sh` 展开 4 个组合，不触发 build/benchmark。
- `./scripts/sync_to_vm.sh` 通过。
- VM 单组合非 profile wrapper smoke 通过：`COMPILER2026_TUNE_THREAD_LIST=1 COMPILER2026_TUNE_ASYNC_MIN_B_LIST=24 COMPILER2026_TUNE_TASK_BATCH_LIST=auto COMPILER2026_TUNE_LABEL_PREFIX=tune_wrapper_smoke REPEAT=1 ./submission/scripts/tune_params.sh`，归档为 `docs/benchmark_results/tune_wrapper_smoke_aggregate.csv`；summary 为 `threads=1 async_min_b=24 task_batch=auto runs=4 serial_total=0.978023s contestant_total=0.963611s speedup_geo=1.009x dag_missing_deps=0`。
- VM 单组合 profile wrapper smoke 通过：`COMPILER2026_TUNE_THREAD_LIST=4 COMPILER2026_TUNE_ASYNC_MIN_B_LIST=24 COMPILER2026_TUNE_TASK_BATCH_LIST=auto COMPILER2026_TUNE_LABEL_PREFIX=tune_wrapper_profile_smoke COMPILER2026_TUNE_PROFILE=1 REPEAT=1 ./submission/scripts/tune_params.sh`，归档为 `docs/benchmark_results/tune_wrapper_profile_smoke_aggregate.csv`；summary 为 `threads=4 async_min_b=24 task_batch=auto runs=4 serial_total=0.979418s contestant_total=0.630574s speedup_geo=1.545x dag_missing_deps=0`，profile summary 包含 `enabled=22 disabled=17 small_b=17`、`runtime_batch_max=8`。
- `./submission/scripts/package.sh` 在 VM 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-08 async threshold 18 and madd output elision

改动：

- 默认 `COMPILER2026_ASYNC_MIN_B` 从 `24` 降到 `18`，runtime、`smoke_test.sh` 和 `benchmark.sh` 的默认值同步。
- Pass 对当前 panel-local DAG 中的 `madd` submit 使用 output key `-1`；`madd` 仍依赖两个 `trsm` 输入 key，但不再把自身输出写入 runtime `latest_producer_` 表。
- `trsm` submit 继续记录 output key，仍由 runtime 释放依赖它的 `madd` task。
- 同步更新 README、设计、性能、路线图和优化原理文档，并归档 `async_min_b18_madd_no_output_default_repeat3.csv` / `async_min_b18_madd_no_output_default_profile_smoke.csv`。

经验：

- 当前 panel 末尾仍有 wait，因此 `madd` 输出在当前 DAG 作用域内没有后续 async consumer；不记录这类 output key 能减少无收益的 producer 表更新。后续做跨 panel DAG 时必须重新纳入相关 `madd` 输出依赖，不能把 `-1` 当成永久语义。
- 单独缓存 hot-path profiling flag 的实验没有形成收益：`profile_flag_hotpath_repeat3` 为 `speedup_geo=1.531x`，低于当前正式 `1.559x`，该改动未保留。
- `b >= 18` 会多启用公开 case 中的 `b=18/20` 等矩阵；在当前 VM 上收益主要来自 `n512_576` 和 `n1152_small_b` 的小 block 区间。它仍是环境相关默认，真实目标机应继续用 `tune_params.sh` 扫参确认。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/tune_params.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在默认阈值下 verifier 通过，speedup `1.600x`。
- `LABEL=async_min_b18_madd_no_output_default_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/async_min_b18_madd_no_output_default_repeat3.csv`；summary 为 `runs=12 serial_total=2.981399s contestant_total=1.801099s speedup_avg=1.655x speedup_geo=1.635x`，IR 计数仍为 `submit_deps=2 submit_plain=0 wait_calls=1 trsm_calls=2 madd_calls=2`。
- `LABEL=async_min_b18_madd_no_output_default_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/async_min_b18_madd_no_output_default_profile_smoke.csv`；async decision 为 `enabled=24 disabled=15 small_b=15 small_blocks=0 threads_disabled=0 single_block=0`，profile summary 包含 `runtime_batch_max=8`、`dag_missing_deps=0`。
- 优化后 IR 检查确认 `trsm` submit output key 仍为动态 block key，`madd` submit output key 为 `i32 -1`，`compiler2026_task_trsm` 直接调用 `@trsm`，`compiler2026_task_madd` 直接调用 `@madd`。
- `./submission/scripts/package.sh` 在 VM 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解压后 CMake/Ninja 构建通过。

## 2026-06-08 opt-in cross-panel DAG experiment

改动：

- Pass 新增 `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1` 实验入口；默认不设置时仍走 panel-local ready queue DAG。
- 实验入口中，Pass 生成 `compiler2026_task_cholesky`，task function 直接调用官方 `@cholesky`；`trsm/madd` task 仍直接调用官方 `@trsm/@madd`。
- runtime 新增通用 `compiler2026_runtime_submit_deps3`，内部 `submitWithDeps` 改为接收依赖数组，支持三输入依赖并去重；runtime 仍只理解 task function、context 和整数 block key。
- 跨 panel 路径中，`cholesky` 依赖当前对角块 previous producer 并输出对角块；`trsm` 依赖目标块和对角块并输出目标块；`madd` 依赖两个 `trsm` 输入块和自身输出块 previous producer，并输出被更新块。
- panel 内静态 wait 被降为外层 DAG 收尾 wait，避免每个 panel 后都强制等待全部 trailing updates。

经验：

- profile 基础设施已经足够支撑大方向优化判断：IR call site、async decision、DAG live/edge/missing-dep、queue/wait 统计和 verifier 已能说明跨 panel 路径的主要瓶颈。
- 直接构建完整跨 panel DAG 在 4 vCPU VM 上没有成为性能收益，原因是 live DAG 和 ready queue 压力显著放大；下一步应做 live-windowing、关键块优先释放或 per-worker queue/work stealing，而不是继续细化无关 profile 字段。
- `dag_missing_deps=7595` 在跨 panel 模式中包含 first-touch/no previous producer 情况，不能直接当作 correctness error；后续 profile 语义应区分“允许的 first touch”和“本应存在但缺失的 producer”。
- 默认路径必须保持 panel-local。跨 panel 实验通过 verifier 但性能低于当前默认，因此只作为研发入口保留。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/tune_params.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `./scripts/sync_to_vm.sh` 通过。
- `./submission/scripts/build.sh` 在 VM 通过。
- `SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.607x`。
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` verifier 通过，speedup `1.542x`。
- `LABEL=cross_panel_gate_default_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 已归档为 `docs/benchmark_results/cross_panel_gate_default_repeat3.csv`；summary 为 `speedup_geo=1.633x`，IR 计数为 `submit_deps=2 submit_plain=0 wait_calls=1 trsm_calls=2 madd_calls=2`，证明默认 gate 没有切到实验路径。
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 LABEL=cross_panel_opt_in_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/cross_panel_opt_in_profile_smoke.csv`；summary 为 `speedup_geo=1.499x`，IR 计数为 `submit_deps=3 submit_plain=0 wait_calls=1 trsm_calls=2 madd_calls=2`，profile 包含 `dag_missing_deps=7595`、`max_dag_live=6072`、`queue_ms_avg=4441.029`。

## 2026-06-08 successor edge pool

改动：

- runtime 的 DAG successor 存储从每个 `DagNode` 一个 `std::vector<int>` 改为统一的 `successor_edges_` 连续 edge pool。
- 每个 producer node 保存 `first_successor`、`last_successor` 和 `successor_count`，新增依赖边时追加到 edge pool 并维护链表尾指针，保留原有 successor 提交顺序。
- `resetQueue()` 按 panel task 预估值额外预留约 `2 * reserve_tasks` 条 edge 容量；`wait()` 清理 panel-local DAG 时同步清理 edge pool。

经验：

- 当前 DAG 的高 fanout 主要来自 `trsm` producer 释放大量 `madd` consumer。把 successor 边集中到连续数组能减少小 vector 分配和扩容，比继续堆 profile 字段更直接作用于调度热路径。
- 修改 aggregate initialization 时要格外小心。第一版保留了旧的尾部 `{}`，把 `first_successor` 初始化成 `0` 而不是默认 `-1`，导致无 successor 节点错误遍历 edge 0；`SPEC_START=91 SPEC_END=104` verifier 捕捉到 `n=1152,b=18` 失败。修正为只初始化前四个字段后 verifier 通过。
- profile 模式下 `dag_edges` 和 `dag_satisfied_deps` 会受提交线程和 worker 完成速度交错影响；判断这类调度结构优化时，应以非 profile `REPEAT=3` 时间、verifier、IR 计数和 `dag_missing_deps=0` 一起作为证据。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/tune_params.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过，speedup `1.638x`。
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过，speedup `1.597x`。
- `LABEL=successor_edge_pool_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/successor_edge_pool_repeat3.csv`；summary 为 `runs=12 serial_total=2.973509s contestant_total=1.780286s speedup_avg=1.663x speedup_geo=1.645x`，IR 计数仍为 `submit_deps=2 submit_plain=0 wait_calls=1 trsm_calls=2 madd_calls=2`。
- `LABEL=successor_edge_pool_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/successor_edge_pool_profile_smoke.csv`；summary 为 `speedup_geo=1.523x`，async decision 为 `enabled=24 disabled=15 small_b=15 small_blocks=0 threads_disabled=0 single_block=0`，profile 包含 `runtime_batch_max=8`、`dag_missing_deps=0`、`max_dag_successors=46`、`max_dag_live=635`。

## 2026-06-08 technical scheme notes and rejected producer-table experiments

改动：

- 新增 `docs/technical_scheme_notes.md`，把官方技术方案 PDF 中和工程实现直接相关的规则、评分、平台、提交物和分块 Cholesky 依赖关系拆成可检索文本。
- `skills/compiler-contest-assistant/SKILL.md` 引用该笔记，并把不修改官方算子、保持 runtime 通用、优先跨 panel 依赖恢复等约束加入项目工作指引。
- 未保留 runtime producer-table 改动：试过把 latest-producer 改为 dense vector，也试过只按当前 panel producer 数预留 unordered_map bucket。

经验：

- official notes 应作为后续路线判断的规则来源，特别是“LLVM Pass 分析依赖 + runtime 调度”与“不可替换官方算子”的边界。
- dense vector producer table 在当前默认 panel-local DAG 上没有收益：它避免了哈希查找，但引入 touched-key 清理和额外状态维护；两组 repeat=3 的 contestant total 分别为 `1.795982s` 和 `1.798338s`，都差于上一轮 `successor_edge_pool_repeat3` 的 `1.780286s`。
- 缩小 unordered_map reserve 到当前 panel producer 数也没有收益，`producer_reserve_repeat3` contestant total 为 `1.798080s`。当前较大的 bucket reserve 虽然占用更多桶，但降低了依赖查找路径的负载因子。
- VM 根分区会被 benchmark 工作目录撑满；本轮 `build/optimization_benchmarks` 达到 `54G` 导致 baseline 写结果失败。清理该生成目录后 benchmark 恢复正常。后续长时间 sweep 前应确认磁盘空间，CSV 归档到 `docs/benchmark_results/` 后可以清理 VM benchmark 工作目录。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/tune_params.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 dense producer table 实验下 verifier 通过，speedup `1.622x`，但正式 repeat=3 未形成 contestant time 收益，改动已回退。
- `LABEL=dense_producer_table_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，summary 为 `contestant_total=1.795982s speedup_geo=1.709x`；高 geomean 主要来自 serial time 波动，不作为保留依据。
- `LABEL=dense_producer_table_repeat3_b REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，summary 为 `contestant_total=1.798338s speedup_geo=1.623x`。
- `LABEL=producer_reserve_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 清理空间后通过，summary 为 `contestant_total=1.798080s speedup_geo=1.658x`，改动已回退。
- `./submission/scripts/package.sh` 在 VM 通过，`dist/submission.zip` 包含 `docs/technical_scheme_notes.md`，并在 `/tmp/judge_zip_test` 解包后 CMake/Ninja 构建通过。

## 2026-06-08 benchmark artifact cleanup

改动：

- `submission/scripts/benchmark.sh` 新增 `COMPILER2026_BENCH_KEEP_ARTIFACTS`。
- 默认值为 `0`：成功生成 CSV 和 summary 后，删除 `${BENCH_DIR}/${LABEL}` 下的 per-suite input/output/profile/verifier 工作目录，保留 `${BENCH_DIR}/${LABEL}.csv`、`bin/` 和 `ir/`。
- 设置 `COMPILER2026_BENCH_KEEP_ARTIFACTS=1` 时保留 per-run artifacts，用于调试 verifier 失败或 profile stderr。
- 失败路径不做清理；脚本中途退出时仍保留现场，便于定位错误。

经验：

- 真实 sweep 不是只看单次 benchmark；大量 label 会快速积累输入矩阵和输出矩阵。本轮 VM 上 `build/optimization_benchmarks` 曾占用 `54G`，导致 baseline 写结果失败。默认清理临时 suite 目录能让长期调参更可靠。
- CSV 已经包含 timing、IR 计数、async decision 和 runtime profile 聚合；成功路径默认保留 CSV 足以支撑性能结论。需要逐 case 复查时再显式打开 `COMPILER2026_BENCH_KEEP_ARTIFACTS=1`。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/tune_params.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `LABEL=benchmark_artifact_cleanup_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/benchmark_artifact_cleanup_smoke.csv`；脚本输出 `cleaned_artifacts=/root/bisheng/build/optimization_benchmarks/benchmark_artifact_cleanup_smoke`，且命令验证 CSV 存在、同名 artifact 目录不存在。
- `LABEL=benchmark_artifact_keep_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 COMPILER2026_BENCH_KEEP_ARTIFACTS=1 ./submission/scripts/benchmark.sh` 在 VM 通过，脚本输出 `kept_artifacts=...`，命令验证同名 artifact 目录存在，目录大小约 `633M`；验证后手动删除该临时目录。
- `./submission/scripts/package.sh` 在 VM 通过，`submission.zip` 在 `/tmp/judge_zip_test` 解包后 CMake/Ninja 构建通过。

## 2026-06-08 benchmark percentile summary

改动：

- `submission/scripts/benchmark.sh` 的 terminal summary 在 suite 分组和 overall 分组中增加 `speedup_p50` / `speedup_p95`。
- CSV schema 不变；percentile 只用于人读 summary，避免为单次格式改动扩大数据面。
- 同步 README、路线图和性能记录，并归档 `docs/benchmark_results/benchmark_percentile_summary_smoke.csv`。

经验：

- 当前 profile 基础设施已经足够支撑大方向判断；P50/P95 属于 benchmark 汇总可读性补强，不应继续演化成更多热路径 profile 字段。
- 试过把 cached `profiling` bool 传入 `completeDagTasksLocked`，减少完成路径中的 atomic load，但在当前默认路径上没有收益；`release_profile_flag_repeat3` 的 `contestant_total=1.856469s`、`speedup_geo=1.612x`，差于 `successor_edge_pool_repeat3` 的 `1.780286s`，改动未保留。

验证：

- `LABEL=benchmark_percentile_summary_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/benchmark_percentile_summary_smoke.csv`；overall summary 输出 `speedup_p50=1.707x`、`speedup_p95=1.879x`。
- `SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 cached profiling flag 实验前通过 verifier，speedup `1.626x`。
- `LABEL=release_profile_flag_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过但性能未达保留标准，改动已回退。

## 2026-06-08 cross-panel live-window drain

改动：

- runtime 新增 `COMPILER2026_DAG_MAX_LIVE`，默认 `0`。非零时，`submitWithDeps` 在 live DAG 超过窗口且 ready queue 非空时，由提交线程执行一小批 ready task 后继续提交。
- 该机制不理解 `cholesky/trsm/madd` 语义，只使用通用 DAG 节点、ready queue 和依赖释放逻辑；默认 panel-local 路径不设置该变量，因此不改变默认调度语义。
- `smoke_test.sh`、`benchmark.sh` 和 `tune_params.sh` 透传 `COMPILER2026_DAG_MAX_LIVE`，便于在目标平台上离线 sweep。
- 同步设计、性能、路线图和 README，归档默认 repeat=3、cross-panel window 候选和 profile CSV。

经验：

- live-window drain 能实质压低跨 panel DAG 的 live pressure：`COMPILER2026_DAG_MAX_LIVE=2048` 的 profile 中 `max_dag_live=2050`，低于早先完整跨 panel DAG 的 `6072`。
- 这还没有解决 4 vCPU VM 上的核心瓶颈。`2048` final smoke 的 `contestant_total=0.611483s`、`speedup_geo=1.598x`，profile smoke 的 `contestant_total=0.658224s`、`speedup_geo=1.550x`，均低于默认 panel-local repeat=3。
- 当前默认正式结果更新为 `live_window_default_repeat3_final`，仍是 panel-local 路径：`contestant_total=1.769841s`、`speedup_geo=1.657x`，IR 计数保持 `submit_deps=2 submit_plain=0 wait_calls=1 trsm_calls=2 madd_calls=2`。
- 后续大方向应继续做 per-worker queue/work stealing、关键块优先或跨 panel 依赖的 live-window 构图策略；只靠提交端 drain 不足以让完整跨 panel DAG 默认化。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/tune_params.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过，speedup `1.616x`。
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_DAG_MAX_LIVE=2048 SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过，speedup `1.615x`。
- `LABEL=live_window_default_repeat3_final REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/live_window_default_repeat3_final.csv`；summary 为 `runs=12 serial_total=2.982439s contestant_total=1.769841s speedup_avg=1.674x speedup_geo=1.657x speedup_p50=1.712x speedup_p95=1.934x`。
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_DAG_MAX_LIVE=<512|1024|2048|4096> LABEL=cross_panel_live*_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，候选 CSV 已归档；最佳候选未超过默认 repeat=3，因此未切默认。
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_DAG_MAX_LIVE=2048 COMPILER2026_DAG_PROFILE=1 LABEL=cross_panel_live2048_profile_final_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/cross_panel_live2048_profile_final_smoke.csv`；profile 包含 `max_dag_live=2050`、`max_wait_dag_live=2046`、`dag_missing_deps=7595`。

## 2026-06-08 tune wrapper min-block and live-window dimensions

改动：

- `benchmark.sh` 的 CSV 新增 `dag_max_live` 字段，用于记录本次运行是否启用了 live-window drain；终端 summary 逻辑不变。
- `tune_params.sh` 新增 `COMPILER2026_TUNE_ASYNC_MIN_BLOCKS_LIST` 和 `COMPILER2026_TUNE_DAG_MAX_LIVE_LIST`，与既有 `async_min_b`、`task_batch`、线程数一起组合 sweep。
- 新增维度默认分别取当前 `COMPILER2026_ASYNC_MIN_BLOCKS` / `COMPILER2026_DAG_MAX_LIVE` 单值，因此默认 sweep 规模不扩大；只有显式设置列表时才探索更多组合。
- `tune_summary` 分组从 `threads + async_min_b + task_batch` 扩展为 `threads + async_min_b + async_min_blocks + dag_max_live + task_batch`。

经验：

- 参数优化确实和执行环境相关；本轮复测显示当前 4 vCPU VM 上 `task_batch=4/16`、`async_min_b=20/22` 和 `threads=5` 都没有超过当前默认。调参工具应覆盖这些维度，但默认不应因为单次实验随意改动。
- 尝试过在 live-window 模式下按 output key 给 ready queue 做优先级，`COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_DAG_MAX_LIVE=2048 SPEC_START=91 SPEC_END=104` smoke 虽通过 verifier，但 speedup 降到 `1.316x`，说明线性 block key 不是可靠关键路径优先级，改动未保留。
- 尝试过把 DAG release 后的 `notify_all` 改为按 released 数量 `notify_one`，并把通知移出 mutex；两组 repeat=3 的 contestant total 分别为 `1.771000s` 和 `1.786654s`，没有稳定超过 `live_window_default_repeat3_final` 的 `1.769841s`，改动未保留。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/tune_params.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- 本地 dry-run 通过：`COMPILER2026_TUNE_DRY_RUN=1 COMPILER2026_TUNE_THREAD_LIST=1,4 COMPILER2026_TUNE_ASYNC_MIN_B_LIST=18 COMPILER2026_TUNE_ASYNC_MIN_BLOCKS_LIST=2,3 COMPILER2026_TUNE_DAG_MAX_LIVE_LIST=0,2048 COMPILER2026_TUNE_TASK_BATCH_LIST=auto REPEAT=1 ./submission/scripts/tune_params.sh` 展开 4 个组合，不触发 build/benchmark。
- VM 单组合 wrapper smoke 通过：`COMPILER2026_TUNE_THREAD_LIST=4 COMPILER2026_TUNE_ASYNC_MIN_B_LIST=18 COMPILER2026_TUNE_ASYNC_MIN_BLOCKS_LIST=2 COMPILER2026_TUNE_DAG_MAX_LIVE_LIST=0 COMPILER2026_TUNE_TASK_BATCH_LIST=auto COMPILER2026_TUNE_LABEL_PREFIX=tune_blocks_live_smoke REPEAT=1 ./submission/scripts/tune_params.sh`，归档为 `docs/benchmark_results/tune_blocks_live_smoke_aggregate.csv`；summary 为 `threads=4 async_min_b=18 async_min_blocks=2 dag_max_live=0 task_batch=auto runs=4 serial_total=0.992355s contestant_total=0.588954s speedup_geo=1.649x dag_missing_deps=0`。
- `LABEL=tune_dims_default_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/tune_dims_default_repeat3.csv`；summary 为 `runs=12 serial_total=2.934934s contestant_total=1.764788s speedup_avg=1.654x speedup_geo=1.637x speedup_p50=1.673x speedup_p95=1.932x`，用于验证新 CSV schema，不替代当前最佳 geomean 记录。

## 2026-06-08 DAG first-touch dependency profile

改动：

- runtime profile 新增 `dag_first_touch_deps`，用于统计依赖 key 尚未由当前 DAG 中任何 task 输出过的缺失 producer。
- `dag_missing_deps` 保留原有总数语义；`dag_first_touch_deps` 是其中可解释为原始输入块 first-touch 的子集。
- 该统计只在 `COMPILER2026_DAG_PROFILE=1` 下维护 `profile_output_keys_`，默认计时路径不做额外 set 查询。
- `benchmark.sh` CSV 和 profile summary 解析同步新增 `dag_first_touch_deps` 字段。

经验：

- 之前跨 panel profile 中 `dag_missing_deps=7595` 容易被误读为依赖丢失。新字段显示 `dag_first_touch_deps=7595`，说明这些缺失 producer 全部来自原始输入块 first-touch，不是 runtime 在同一 DAG 中遗失 producer。
- 这不改变调度行为，也不让 cross-panel 默认化；它让下一步判断跨 panel 依赖质量时能区分“合法 first touch”和“真正异常 missing producer”。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/tune_params.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过，speedup `1.635x`。
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_DAG_MAX_LIVE=2048 COMPILER2026_DAG_PROFILE=1 LABEL=dag_first_touch_cross_panel_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/dag_first_touch_cross_panel_profile_smoke.csv`；summary 包含 `dag_missing_deps=7595 dag_first_touch_deps=7595`。

## 2026-06-08 cross-panel sync cholesky key wait

改动：

- 新增实验开关 `COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1`，必须和 `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1` 一起使用。
- Pass 在该路径中保留原始同步 `cholesky` ABI 调用，不再把 `cholesky` 提交为 DAG task；每个 `cholesky` 前插入通用 `compiler2026_runtime_wait_key(diagonal_input_key)`。
- `trsm` 和 `madd` 仍使用跨 panel dependency submit，`madd` 保持三输入依赖和 output key，用于表达自身输出块 previous producer。
- runtime 新增通用 `waitForKey` / `compiler2026_runtime_wait_key`，只等待整数 key 的 latest producer，等待期间主线程会执行 ready task；该函数标记为 cold/noinline，默认 panel-local IR 不声明也不调用 `wait_key`。
- `compiler2026_task_cholesky` 改为按需创建，只有旧的 taskized cross-panel 实验路径需要时才生成；sync-cholesky 路径中 IR 不再包含 cholesky task 引用。

经验：

- 该路径把跨 panel 实验的 live DAG 压力进一步压低：`cross_panel_sync_cholesky_profile_final_smoke.csv` 中 `max_dag_live=1066`，低于 taskized cross-panel + live-window 的约 `2050`。
- 这仍没有超过默认 panel-local 路径：sync-cholesky profile smoke 的 `speedup_geo=1.534x`、`contestant_total=0.660494s`，明显低于当前默认正式记录。因此它保留为 opt-in 实验，不切默认。
- 默认路径 guard repeat=3 为 `contestant_total=1.778253s`、`speedup_geo=1.641x`，低于当前最佳正式记录 `live_window_default_repeat3_final` 的 `contestant_total=1.769841s`、`speedup_geo=1.657x`；当前最佳配置不变。
- 这轮结果说明减少 cholesky task 化和 live 节点数量有帮助，但主要瓶颈仍在跨 panel 依赖维护、单全局 ready queue 和关键路径调度，而不是 cholesky task 本身。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/tune_params.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过，speedup `1.630x`；默认 IR 检查为 `wait_key_refs=0`、`cholesky_task_refs=0`。
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1 COMPILER2026_DAG_MAX_LIVE=2048 SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过；实验 IR 检查为 `wait_key=1`、`cholesky_task_refs=0`、`submit_deps3=1`。
- `LABEL=sync_cholesky_default_guard_repeat3_final REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/sync_cholesky_default_guard_repeat3_final.csv`。
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1 COMPILER2026_DAG_MAX_LIVE=2048 COMPILER2026_DAG_PROFILE=1 LABEL=cross_panel_sync_cholesky_profile_final_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/cross_panel_sync_cholesky_profile_final_smoke.csv`。
- `./submission/scripts/package.sh` 在 VM 通过，`dist/submission.zip` 在 `/tmp/judge_zip_test` 解包后 CMake/Ninja 构建通过；zip 已同步回本地 `dist/submission.zip`。

## 2026-06-08 rejected cross-panel reserve sizing

尝试：

- runtime 的 `reserveTaskCount(n, b)` 从 panel-local 首 panel容量估算扩展为 cross-panel aware 估算。
- 当 `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1` 时，按整轮 block DAG 的 `cholesky + trsm + madd` 上界预留 `dag_nodes_`，按依赖数上界预留 `successor_edges_`，按 lower-triangular block 数预留 producer 表。
- 为避免 hidden 大规模小 block case 一次性预留过大，实验实现对 nodes/edges/producers reserve 分别加了上限。

结果：

- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_DAG_MAX_LIVE=2048 COMPILER2026_DAG_PROFILE=1 LABEL=cross_panel_reserve_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/cross_panel_reserve_profile_smoke.csv`；summary 为 `contestant_total=0.663732s speedup_geo=1.503x max_dag_live=2065`。
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1 COMPILER2026_DAG_MAX_LIVE=2048 COMPILER2026_DAG_PROFILE=1 LABEL=cross_panel_sync_cholesky_reserve_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/cross_panel_sync_cholesky_reserve_profile_smoke.csv`；summary 为 `contestant_total=0.662763s speedup_geo=1.532x max_dag_live=1056`。

结论：

- 这两个结果都没有超过对应旧实验记录；尤其 sync-cholesky 旧 profile smoke 为 `contestant_total=0.660494s speedup_geo=1.534x`。
- 预留容量能减少一部分扩容/rehash 可能性，但当前 4 vCPU VM 上 cross-panel 的主瓶颈仍是依赖维护、ready queue 调度和 wait/queue pressure，不是 vector/hash 初始容量。
- 代码改动已回退，只保留 CSV 和日志，避免把无收益的 reserve complexity 留在 runtime 默认框架里。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/tune_params.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- 回退后 `submission/runtime/dag_runtime.cpp` 无未提交 diff。

## 2026-06-08 worker pinning experiment

改动：

- runtime 新增默认关闭的 `COMPILER2026_DAG_PIN_WORKERS=1`。
- Linux 下启用该开关时，worker 线程读取当前进程 affinity mask，并按可用 CPU 列表轮转绑定；非 Linux 或 affinity 读取失败时自动退化为不绑定。
- `smoke_test.sh` 和 `benchmark.sh` 透传该开关，benchmark CSV 新增 `dag_pin_workers` 字段。
- `tune_params.sh` 的 aggregate summary 将 `dag_pin_workers` 纳入分组，避免 pinning 与非 pinning 结果混合。

经验：

- 在 4 vCPU VM 上，pinning repeat=3 略好于同轮默认 guard，但没有超过当前最佳正式记录；因此不切默认。
- worker 亲和性属于强环境相关优化。真实鲲鹏/NUMA 平台上仍值得和线程数、NUMA 绑定一起离线测试，但 contestant 默认路径保持不绑定，避免对未知调度环境过拟合。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/tune_params.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- 本地 dry-run 通过：`COMPILER2026_TUNE_DRY_RUN=1 COMPILER2026_TUNE_THREAD_LIST=1,4 COMPILER2026_TUNE_ASYNC_MIN_B_LIST=18 COMPILER2026_TUNE_ASYNC_MIN_BLOCKS_LIST=2 COMPILER2026_TUNE_DAG_MAX_LIVE_LIST=0 COMPILER2026_TUNE_TASK_BATCH_LIST=auto REPEAT=1 ./submission/scripts/tune_params.sh`。
- `SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过，speedup `1.618x`。
- `SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PIN_WORKERS=1 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过，speedup `1.632x`。
- `LABEL=pin_workers_schema_default_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/pin_workers_schema_default_repeat3.csv`；summary 为 `contestant_total=1.790583s speedup_geo=1.633x`。
- `COMPILER2026_DAG_PIN_WORKERS=1 LABEL=pin_workers_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/pin_workers_repeat3.csv`；summary 为 `contestant_total=1.775266s speedup_geo=1.646x`。
- `COMPILER2026_DAG_PIN_WORKERS=1 COMPILER2026_DAG_PROFILE=1 LABEL=pin_workers_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/pin_workers_profile_smoke.csv`。
- `./submission/scripts/package.sh` 在 VM 通过，`dist/submission.zip` 在 `/tmp/judge_zip_test` 解包后 CMake/Ninja 构建通过；zip 已同步回本地 `dist/submission.zip`。

## 2026-06-08 tune wrapper pinning dimension

改动：

- `tune_params.sh` 新增 `COMPILER2026_TUNE_DAG_PIN_WORKERS_LIST`。
- 默认值继承当前 `COMPILER2026_DAG_PIN_WORKERS`，未设置时为单值 `0`，因此日常 sweep 规模不增加。
- 显式设置 `COMPILER2026_TUNE_DAG_PIN_WORKERS_LIST=0,1` 时，wrapper 会分别调用 `benchmark.sh` 并把 `COMPILER2026_DAG_PIN_WORKERS` 透传给 contestant。
- label 中加入 `pin<0|1>`，aggregate summary 继续按 `dag_pin_workers` 分组。

经验：

- worker pinning 是环境相关参数，应该纳入目标机事前离线搜索，而不是要求手工跑两套 benchmark 后再拼 CSV。
- 新维度默认单值，避免和 async 阈值、batch、live-window 组合后无意放大日常 sweep 成本。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/tune_params.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- 本地 dry-run 通过：`COMPILER2026_TUNE_DRY_RUN=1 COMPILER2026_TUNE_THREAD_LIST=1,4 COMPILER2026_TUNE_ASYNC_MIN_B_LIST=18 COMPILER2026_TUNE_ASYNC_MIN_BLOCKS_LIST=2 COMPILER2026_TUNE_DAG_MAX_LIVE_LIST=0 COMPILER2026_TUNE_DAG_PIN_WORKERS_LIST=0,1 COMPILER2026_TUNE_TASK_BATCH_LIST=auto REPEAT=1 ./submission/scripts/tune_params.sh`，展开 pin0/pin1 两个组合。

## 2026-06-08 rejected fanout-priority ready dequeue

尝试：

- 在 runtime 中加入默认关闭的 ready dequeue fanout priority：取 ready task 时扫描小窗口，优先执行 successor fanout 更高的 DAG node。
- 该策略只使用 runtime 已有的通用 successor 计数，不读取 task 名称，也不把 `trsm/madd/cholesky` 语义写入 runtime。
- `smoke_test.sh`、`benchmark.sh` 和 `tune_params.sh` 临时透传并记录该开关，用于验证是否值得保留为跨 panel 调度实验维度。

结果：

- `SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过，speedup `1.601x`。
- `SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_FANOUT_PRIORITY=1 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过，但 speedup 降到 `1.435x`。
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_DAG_MAX_LIVE=2048 COMPILER2026_DAG_FANOUT_PRIORITY=1 SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过，speedup `1.409x`。
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_DAG_MAX_LIVE=2048 COMPILER2026_DAG_FANOUT_PRIORITY=1 COMPILER2026_DAG_PROFILE=1 LABEL=cross_panel_fanout_priority_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/cross_panel_fanout_priority_profile_smoke.csv`；summary 为 `contestant_total=0.695125s speedup_geo=1.452x max_dag_live=2049`。

结论：

- fanout priority 同时伤害默认 panel-local smoke 和 cross-panel live-window profile smoke，没有达到保留 opt-in 代码的门槛。
- 代码和脚本改动已回退，只保留 CSV 和日志。后续若继续做关键路径优先，应由 Pass 提供更精确的 critical-path rank 或改成 per-worker queue/work stealing，而不是只按局部 successor fanout 排序。

## 2026-06-08 sync-cholesky live-window repeat benchmark

验证：

- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1 COMPILER2026_DAG_MAX_LIVE=2048 LABEL=cross_panel_sync_cholesky_live2048_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/cross_panel_sync_cholesky_live2048_repeat3.csv`。
- summary 为 `runs=12 serial_total=2.894289s contestant_total=1.828356s speedup_avg=1.614x speedup_geo=1.587x speedup_p50=1.625x speedup_p95=1.941x`。

经验：

- sync-cholesky + live-window 确实比早期完整 cross-panel DAG 更受控，但 repeat=3 仍没有超过当前默认 `live_window_default_repeat3_final` 的 `contestant_total=1.769841s`、`speedup_geo=1.657x`。
- 该路径继续保留为 opt-in 实验入口，不默认启用。后续跨 panel 方向应优先解决单全局队列、依赖维护和关键路径调度，而不是继续围绕 cholesky task 化本身调参。

## 2026-06-08 wait-key completion notification

改动：

- `runtime_wait_key` 不再用 `done_cv.wait_for(10us)` 超时轮询等待 key producer 完成。
- runtime 新增 `key_waiters_` 计数；只有存在 key waiter 时，worker 或提交线程完成 ready batch 后才额外通知 `done_cv`。
- 默认 panel-local 路径不调用 `runtime_wait_key`，因此没有 key waiter；该改动主要服务 sync-cholesky cross-panel 实验路径。

经验：

- 目标实验路径有小幅改善：`key_wait_notify_sync_cholesky_live2048_repeat3.csv` 为 `contestant_total=1.796239s`、`speedup_geo=1.606x`，好于上一轮同配置 `cross_panel_sync_cholesky_live2048_repeat3.csv` 的 `contestant_total=1.828356s`、`speedup_geo=1.587x`。
- 默认 guard 本轮为 `contestant_total=1.753659s`、`speedup_geo=1.616x`，不替代当前最佳正式记录。当前最佳仍是 `live_window_default_repeat3_final`。
- wait-key 通知能降低实验路径的等待发现延迟，但不足以解决 cross-panel 的主要瓶颈；后续仍应转向单全局队列、依赖维护和更精确的关键路径调度。

验证：

- `bash -n submission/scripts/build.sh submission/scripts/smoke_test.sh submission/scripts/benchmark.sh submission/scripts/tune_params.sh submission/scripts/package.sh scripts/sync_to_vm.sh` 通过。
- `git diff --check` 通过。
- `SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过，speedup `1.627x`。
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1 COMPILER2026_DAG_MAX_LIVE=2048 SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过，speedup `1.584x`。
- `LABEL=key_wait_notify_default_guard_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/key_wait_notify_default_guard_repeat3.csv`。
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1 COMPILER2026_DAG_MAX_LIVE=2048 LABEL=key_wait_notify_sync_cholesky_live2048_repeat3 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/key_wait_notify_sync_cholesky_live2048_repeat3.csv`。
- `./submission/scripts/package.sh` 在 VM 通过，`dist/submission.zip` 在 `/tmp/judge_zip_test` 解包后 CMake/Ninja 构建通过；zip 已同步回本地 `dist/submission.zip`。

## 2026-06-08 rejected completed-producer table shrink

尝试：

- runtime DAG node 临时记录 output key。
- 当 task 完成且它仍是某个 key 的 latest producer 时，从 `latest_producer_` 删除该 key。
- 目标是让跨 panel DAG 的 producer 表只保留仍未完成的 producer，减少 completed-producer 查询和 hash table 压力。

结果：

- `COMPILER2026_DAG_DROP_COMPLETED_PRODUCERS=1 SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过，speedup `1.619x`。
- `COMPILER2026_DAG_DROP_COMPLETED_PRODUCERS=1 COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1 COMPILER2026_DAG_MAX_LIVE=2048 SPEC_START=91 SPEC_END=104 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh` 在 VM 通过，verifier 通过，但 speedup 降到 `1.394x`。
- `COMPILER2026_DAG_DROP_COMPLETED_PRODUCERS=1 COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1 COMPILER2026_DAG_MAX_LIVE=2048 COMPILER2026_DAG_PROFILE=1 LABEL=drop_completed_producers_profile_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh` 在 VM 通过，归档为 `docs/benchmark_results/drop_completed_producers_profile_smoke.csv`；summary 为 `contestant_total=0.728694s speedup_geo=1.363x dag_missing_deps=243986 dag_first_touch_deps=8012 max_dag_live=893`。

结论：

- 删除 completed latest producer 虽然降低了 `max_dag_live`，但大量后续依赖会从 satisfied completed producer 变成 missing producer，破坏当前 profile 语义，也没有带来性能收益。
- 代码改动已回退，只保留 CSV 和日志。后续如果要缩 producer 表，应单独维护 completed-producer cache 或 profile 语义，而不是直接 erase latest producer。

## 2026-07-17 annotation-driven tile semantics and ranked ready queue experiment

改动：

- 在提交包的官方 baseline 副本上只增加函数级
  `[[clang::annotate("compiler2026.graph.block_cholesky.tile_dag.v1")]]`；`main.cpp` 原样提供，
  `block_cholesky.cpp` 去除该属性后与 SDK 官方文件一致。
- `package.sh` 把 `src/baseline/` 放入提交包；smoke/benchmark 的 contestant IR 改由这份
  annotated source 生成，串行对照仍编译 SDK 官方源码。
- Pass 在克隆前读取 `llvm.global.annotations`。默认 panel-local 路径不变；跨 panel 实验
  根据 `tile_dag.v1` 的 row-major double / `L` 基址契约，用 pointer difference 恢复
  block 坐标，覆盖 Clang 把 GEP 链折叠成 PHI 的 IR。
- 新增默认关闭的 `COMPILER2026_DAG_CRITICAL_PRIORITY=1`：Pass 为任务生成 `0..3` rank，
  runtime 使用三个通用 priority FIFO 加普通 FIFO。对角更新/cholesky 为 rank 3，首个
  subdiagonal trsm 为 rank 2，其余 trsm 和下一 panel 列更新为 rank 1，其余为 0。
- rank 2/3 单任务出队以尽早释放关键链，rank 1 保留自适应 batch，避免所有 trsm 都按
  batch 1 重新放大全局锁开销。runtime 仍只看整数依赖 key 和 rank，不理解算子名称。
- `benchmark.sh` 新增 `dag_critical_priority`、`ir_submit_priority`、`priority_tasks`、
  `priority_dequeue_batches` 和 `max_priority_ready` 字段，并记录决定 IR 的
  `pass_cross_panel_dag`、`pass_sync_cholesky` 两个编译期开关。
- rank 改用 `uint8_t` 填入 `DagNode::completed` 后的 padding，使 64 位 `DagNode`
  保持原有 40 B；priority 关闭时 enqueue/dequeue 走直接 normal-queue fast path，避免
  已判定无收益的实验扩大默认节点布局。

本地验证：

- 使用 Homebrew LLVM 20.1.3 构建 Pass/runtime 通过；同时修复 async internal clone 在新
  verifier 下需要显式 `dso_local` 的兼容性问题。LLVM 15 原路径语义不变。
- `llvm.global.annotations` 中确认存在 `tile_dag.v1`；默认 IR 无 priority submit，实验 IR
  有 2 个静态 `submit_deps3_priority` call site。
- `SPEC_START=93 SPEC_END=93` 的默认路径、sync-cross-panel 路径和 ranked-priority 路径均
  通过本地 verifier。profile 中 ranked 路径执行 `priority_tasks=992`，说明 annotation、
  Pass rank 和 runtime queue 已真正贯通。
- `local_semantic_priority_schema_final.csv` 的四组 profile benchmark 通过；CSV 每行
  均为 76 列，并验证 `tasks=main+worker`、`priority_tasks<=tasks`、
  `max_priority_ready<=max_ready`。
- 上述四份本机 CSV 已归档至 `docs/benchmark_results/`；新增配置字段后，新生成 CSV 为
  76 列并带有明确的 cross-panel/sync-cholesky 配置值，文档不再依赖被 `.gitignore`
  排除的 `build/` 文件。
- 无 profile、4 线程、repeat=3：无 priority 跨 panel guard 为
  `contestant_total=3.571598s speedup_geo=1.519x`；最终三级 rank + rank-1 batch 为
  `contestant_total=3.720227s speedup_geo=1.414x`；默认 panel-local guard 为
  `contestant_total=3.079379s speedup_geo=1.563x`。
- 因此 priority 实验不默认启用。annotation 驱动的坐标恢复保留为跨 panel opt-in 的
  IR 鲁棒性基础，后续更值得推进 bounded look-ahead 或 per-worker queue，而不是继续调整
  单全局队列中的 rank 常量。
- `bash -n submission/scripts/*.sh scripts/sync_to_vm.sh` 和 `git diff --check` 通过。

限制：

- 本轮结果来自本机 x86_64/LLVM 20，不是正式性能环境。
- `./scripts/sync_to_vm.sh` 因当前账号缺少 `~/.ssh/bisheng_vm_ed25519` 失败；没有声称完成
  openEuler aarch64 / BiSheng 15 benchmark 或打包复核。恢复 VM 密钥后应先跑默认 guard，
  再决定是否值得在 48/64 核目标机重新评估 ranked queue。

## 2026-08-03 source-only judge submission package

交付修正：

- 审计发现 macOS 上运行旧 `package.sh` 会把 Mach-O x86_64 Pass/runtime 复制到 ZIP；官方
  manifest 又优先解析 `submission_dir`，会让这些宿主机产物遮蔽 judge 在 `build_dir` 中
  重新生成的目标产物，因此这种 ZIP 不能提交。
- `package.sh` 改为 source-only：仍先 clean/build 验证工程，但 ZIP 只携带 CMake、manifest、
  Pass/runtime 源码、annotation baseline、脚本和文档，不预置 `.so/.a`。官方 judge 必须在
  自己的目标环境构建，并由 manifest 回退到 `build_dir/pass` 和 `build_dir/runtime`。
- Pass CMake 的自动查找列表补充 `llvm-config-16`、`llvm-config-15`。

本机 LLVM 15.0.7 验证：

- Pass/runtime clean build 通过，`opt -load-pass-plugin ... -passes=contestant-pass` 成功。
- `SPEC_START=1 SPEC_END=150 COMPILER2026_DAG_THREADS=4` 的完整公开用例通过 verifier；
  本机累计 `serial_seconds=15.140461`、`contestant_seconds=8.180619`。这些性能数字仅用于
  运行链路检查，不作为 ARM 正式成绩。
- 从最终 source-only ZIP 解压到空目录，使用 LLVM 15 独立 CMake 构建，再用解压出的
  Pass/runtime 生成 contestant app；同一批 150 个公开用例再次通过 verifier。
- 优化 IR 中确认 `compiler2026_task_trsm` / `compiler2026_task_madd` 仍分别调用官方
  `@trsm` / `@madd`；ZIP CRC、根目录 `CMakeLists.txt`、manifest JSON、annotation 去除后
  与官方 baseline 一致等检查均通过。

限制：

- 当前仍无法登录 `192.168.8.131`：专用 SSH key 不存在，现有 `id_rsa` 在握手阶段被服务端
  关闭。因此本轮交付采用官方模板支持的 source-only 形式，已消除错误宿主二进制，但尚未
  获得 openEuler aarch64 / BiSheng 15 的实机构建与 verifier 证据。

## 2026-08-05 40 物理核平台扫描：定位多核扩展瓶颈（Round 1）

环境说明：

- 新增调试平台：Intel Xeon Gold 5218R ×2，x86_64 / Ubuntu 22.04.5 / glibc 2.35，
  **40 物理核 / 80 逻辑核（SMT2）**，2 NUMA node，L2 1MB/核，L3 27.5MB×2，
  LLVM 17.0.6（无毕昇）。全部实验 `taskset -c 0-39` 绑到 40 个物理核，
  governor 设为 performance。
- 这不是正式性能环境（正式成绩仍以鲲鹏 920 / openEuler aarch64 / 毕昇 15 为准）。
  本轮结论按"结构性扩展性结论"记录，不写入 `performance.md` 的正式结果。
  跨机绝对时间不可比，只比同机加速比。

### 结论 1：默认路径在 8 线程即压平

`benchmark.sh` 四个 suite 全集、REPEAT=3、`COMPILER2026_DAG_THREAD_LIST=1,2,4,8,16,32,40`：

| 线程 | 1 | 2 | 4 | 8 | 16 | 32 | 40 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| geomean | 1.017x | 1.352x | 1.722x | 1.844x | 1.839x | 1.856x | 1.796x |

四个 suite 无一例外在 8 线程压平，40 核相对 8 核没有收益。273 个 verifier 结果全部 PASS。
CSV：`xeon_thread_sweep_default.csv`。

### 结论 2：跨 panel DAG 在 40 核上仍无收益（否证）

`COMPILER2026_ENABLE_CROSS_PANEL_DAG=1` + `COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1`
+ `COMPILER2026_DAG_MAX_LIVE=2048`，同区间对照：

| 线程 | 4 | 8 | 16 | 32 | 40 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 跨 panel | 1.716x | 1.810x | 1.818x | 1.811x | 1.794x |
| 默认 | 1.722x | 1.844x | 1.839x | 1.856x | 1.796x |

全程持平或略低。IR 断言确认开关生效（`cross_panel=1 sync_cholesky=1`，`wait_calls` 由 1 变 2），
195 个 verifier 全部 PASS。CSV：`xeon_crosspanel_sync.csv`。

这否证了此前"panel 末尾 barrier 是大核数首要限制"的假设，也否证了 M5 harness 关于
"跨 panel 收益 ∝ 核数/block 数"的外推。原因见结论 3、4：核根本没有被申请到，
且调度器在 barrier 成为约束之前就已饱和。

### 结论 3：全集低加速比主要来自两重稀释，不是调度器上限

`resolveThreadCount = min(threads, block_count)` 使线程数被 block 数截断。以 n1024 suite 为例，
async 用例是 b=32/64/128/256 → block 数 32/16/8/4 → 分别只能用 32/16/8/4 个线程。
超过 8 线程后大部分用例不再申请更多核，这就是全集在 8 线程压平、且改 barrier 无效的原因。
另有 `b < 18` 的用例走串行路径（n1024 中 2/6，公开全集 49/150），结构性 1.0x。

单用例隔离测试（`n=2048 b=32`，64 个 block，线程上限不绑定，每点 3 次取最小值）：

| 线程 | 1 | 2 | 4 | 8 | 12 | 16 | 20 | 24 | 32 | 40 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 双 socket / 40 核 | 0.92x | 1.73x | 3.55x | 6.67x | — | 11.32x | — | 7.70x | 7.79x | 7.82x |
| 单 socket / 20 核 / 单 NUMA | 0.93x | 1.69x | 3.29x | 6.64x | 9.46x | 11.43x | 9.31x | — | — | — |

同一 runtime 在 block 数充足时能达到 **11.3x**，说明 1.85x 的平台不是调度器能力上限。

### 结论 4：存在真实的 ~16 线程断崖，且与 NUMA 无关

峰值出现在 16 线程（11.32x / 11.43x），之后回退约 30%。单 socket、单 NUMA node、
内存绑定 node0 的对照里峰值位置和高度几乎一致（16 线程 11.43x，20 线程回退到 9.31x），
因此排除跨 socket / 远程访存解释。40 核那组的 24 线程点是 24 个线程跑在 40 个 CPU 上，
不存在超订，回退是真实的。

`COMPILER2026_DAG_PROFILE=1` 给出机制：

| 线程 | 时间 | max_ready | max_batch | dequeue_batches | worker_idle_ms | exec_ms | main tasks |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 0.208s | 1744 | 4 | 13596 | 35 | 1492 | 5187 |
| 16 | 0.130s | 994 | 2 | 29328 | 242 | 1502 | 1194 |
| 24 | 0.188s | 42 | 1 | 45696 | 2425 | 1489 | 18 |
| 40 | 0.226s | 21 | 1 | 45696 | 6491 | 1603 | 17 |

`exec_ms` 基本恒定（有效计算量不变），但 24 线程起 ready queue 几乎为空
（`max_ready` 994 → 42），`worker_idle_ms` 暴涨到 6491ms（40 线程时占可用核时约 74%），
`dequeue_batches` 等于 `tasks` 表示每个 task 单独出队，主线程执行的 task 从 5187 掉到 17。

即：**单线程 DAG 提交是绑定约束**。`submitWithDeps` 对每个 task 单独获取全局 mutex，
提交者与 N 个 worker 争同一把锁，形成正反馈——队列饥饿 → `chooseBatchCount` 退化为 1
→ 每 task 两次加锁 → 锁压力最大化 → 提交更慢 → 队列更饥饿。这解释了为什么是"回退"
而不仅是"压平"：worker 越多，提交者越拿不到锁。

### 本轮代码改动：定向唤醒替代 notify_all

- 新增 `AsyncRuntime::notifyWorkers(count)`：依赖释放和 submit flush 时，按新就绪任务数
  调用对应次数 `notify_one`，仅在 `count >= worker 数` 时才退化为 `notify_all`。
  停机路径（析构器设置 `stopping_` 后）仍保留 `notify_all`，必须唤醒全部 worker 退出。
- 动机：`notify_all` 每次释放事件产生 O(participants) 次 futex 唤醒，而其中最多 `count`
  个线程能取到任务，其余只是重新竞争 mutex、复查谓词后再次 park。
- 欠唤醒是安全的：每个参与者在完成一批后都会重新求值 `hasReadyTasksLocked()`，
  提交侧 flush 也会独立唤醒。

同一隔离用例改前/改后：

| 线程 | 1 | 2 | 4 | 8 | 16 | 24 | 32 | 40 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 改前 | 0.92x | 1.73x | 3.55x | 6.67x | 11.32x | 7.70x | 7.79x | 7.82x |
| 改后 | 1.00x | 1.76x | 3.55x | 6.80x | 11.74x | 8.28x | 7.53x | 8.36x |

经验：

- **惊群不是主因。** 16 线程 +3.7%、40 线程 +6.9%，而 serial 基线本身有约 5% 的
  run-to-run 波动（turbo/热），所以 16 线程的改善基本在噪声内，只有 40 线程略高于噪声。
  断崖位置和深度都没有变化。改动本身是正确方向且无风险，保留。
- 测量方法教训：这台机器 turbo 未锁定，单点差异小于 5% 不足以判定。后续每点至少 3 次
  取最小值，并始终同批测 serial 基线。
- `benchmark.sh` 会运行 verifier 但**从不检查结果**（`:139`），而默认
  `COMPILER2026_BENCH_KEEP_ARTIFACTS=0` 又会删除含 `.verify` 的目录。也就是说按默认参数
  产出的性能数字没有正确性证据。本轮全程 `KEEP_ARTIFACTS=1` 并逐条核对，脚本本身待修。

后续（按预期收益排序）：

1. 批量化 DAG 提交，压缩提交者的锁占用（直接针对结论 4 的根因）。
2. 在新的饱和点上重定 `resolveThreadCount`：当前策略在 `block_count >= 18` 时过量供给
   （B=64 用 40 线程只有 8.36x，用 16 线程有 11.74x），在 `block_count < 18` 时相对
   DAG 平均宽度又偏多。
3. 对 `b < 18` 做 task 粗化，把公开集里 49/150 个串行路径用例带入并行路径。

## 2026-08-05 批量化 DAG 提交（Round 2）

背景：Round 1 的 profile 证明绑定约束是单线程 DAG 提交——`submitWithDeps` 对每个 task
单独获取全局 mutex，与 N 个 worker 争同一把锁，导致队列饥饿的正反馈。

改动：

- `submitWithDeps` 拆成"暂存 + 批量发布"两段。依赖键解析（`latest_producer_` 哈希查找）、
  重复依赖去重、输出键登记全部移出临界区，因为 `dag_nodes_`、`successor_edges_`、
  `latest_producer_` **只有提交线程追加**，worker 只修改已发布节点的 `pending`/`completed`
  并读取 successor 边。
- 新增 `StagedSubmit` 暂存项：保存 task 函数、context、priority，以及已解析为
  producer 节点索引的依赖（`kDepSkip` 表示依赖为负或重复，`kDepMissing` 表示该键没有
  活跃 producer）。节点索引由 `dag_next_node_index_ + dag_staging_.size()` 预测；
  预测索引可以直接写入 `latest_producer_`，因为未发布的节点不可能被完成，后续连边一定
  会变成真实边。
- 新增 `flushDagStaging()`：一次加锁内完成"先追加全部节点 → 再连全部边 → 最后入队就绪
  节点"。这个顺序是正确性关键：若先入队，worker 可能在同批 producer 的 successor 边
  建立之前就完成它，从而丢失依赖。
- `flushPendingTasks()` 先 flush DAG 暂存，因此 `wait()` / `waitForKey()` 入口自动清空暂存。
- `clearCompletedDagStateLocked()` 同步把 `dag_next_node_index_` 归零——它会清空
  `dag_nodes_`，索引预测必须跟着重置，否则会连到错误节点。这是本轮最容易踩的坑。
- 批量大小 `chooseDagStagingLimit()`：`reserve_tasks / (participants * 2)`，上限 32，
  下限 1；`COMPILER2026_DAG_SUBMIT_BATCH` 可覆盖。原则是只有当首个 panel 的工作量明显
  多于"暂存一批期间线程池能消费的量"时才压批，block 数小的场景自动退回逐个发布。
- 开启 `COMPILER2026_DAG_MAX_LIVE` 的实验路径会在每次 submit 后强制 flush，等于关闭批量。
  该路径默认关闭，保持正确性优先。

隔离用例 `n=2048 b=32`（64 blocks，线程上限不绑定，每点 3 次取最小值）：

| 线程 | 1 | 2 | 4 | 8 | 16 | 24 | 32 | 40 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Round 0 | 0.92x | 1.73x | 3.55x | 6.67x | 11.32x | 7.70x | 7.79x | 7.82x |
| Round 1 | 1.00x | 1.76x | 3.55x | 6.80x | 11.74x | 8.28x | 7.53x | 8.36x |
| Round 2 | 0.98x | 1.86x | 3.54x | 6.74x | 12.09x | **14.08x** | 11.98x | 8.42x |

峰值由 16 线程 11.74x 移到 24 线程 **14.08x**（+20%）；24 线程点 +70%，32 线程点 +59%。
`scaled_residual` 与 serial 逐位一致。

全集聚合（`xeon_r2_submit_batch.csv`，REPEAT=3，312 个 verifier 全 PASS）：

| 线程 | 1 | 2 | 4 | 8 | 16 | 24 | 32 | 40 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Round 0 | 1.017x | 1.352x | 1.722x | 1.844x | 1.839x | — | 1.856x | 1.796x |
| Round 2 | 1.011x | 1.349x | 1.691x | 1.875x | **1.956x** | 1.869x | 1.824x | 1.810x |

经验：

- 聚合只从 1.844x 升到 1.956x（+6%），远小于隔离用例的 +20%。原因是聚合仍被
  Round 1 结论 3 的两重稀释压住：`resolveThreadCount` 的 block 数上限，以及 `b < 18`
  的串行路径。**隔离指标的收益要落到聚合上，必须先解除这两个上限。**
- "提交者与 worker 争同一把锁"这类瓶颈在小核数平台上完全不可见：4 核时提交者几乎总能
  拿到锁。这解释了为什么此前所有基于 4-vCPU 的调优都没有触及它。

profile 显示瓶颈已转移（同一隔离用例）：

| 线程 | 时间 | max_ready | max_batch | worker_idle_ms |
| --- | ---: | ---: | ---: | ---: |
| 16 | 0.121s | 1482 | 2 | 110 |
| 24 | 0.103s | 1134 | 2 | 274 |
| 40 | 0.173s | 283 | 1 | 3759 |

队列深度相对 Round 1 提升 13–27 倍，提交饥饿基本解除。但 40 线程时 `max_batch` 被
**策略**压成 1：`selectTaskBatchSize` 的 `blocks <= participants * 2` 规则在
`blocks=64, participants=40` 时命中（64 <= 80）。此时 ready 宽度有 283，批量本可放大，
说明该静态规则用 block 数近似 ready 宽度，在高参与者数下失准；而 `chooseBatchCount`
里的 `available / participants` 已经是正确的动态公平性保护。这是 Round 3 的目标。

## 2026-08-05 移除批量大小的静态 block 数钳制（Round 3）

背景：Round 2 的 profile 显示 40 线程时 `max_batch` 被压成 1，而 ready 宽度有 283。

改动：

- `selectTaskBatchSize` 删除三条基于 block 数的静态钳制
  （`blocks <= participants*2 → 1`、`*4 → 2`、`*8 → 4`），只保留按 tile 粒度选择
  （`b <= 64 → 8`，`b <= 128 → 4`，更大 → 1）。
- 理由：批量大小本质是**任务粒度**决策（小 tile 的队列往返相对其计算量太贵），而**公平性**
  已由 `chooseBatchCount()` 动态保证——它在 ready 宽度不足时返回 1，否则最多发放
  `available / participants`。静态规则是同一个保护的粗糙替代，且在参与者数接近 block 数时
  失准：`blocks=64, participants=40` 时它强制批量 1，而实测 ready 宽度是 283，
  等于在最不该加锁的地方把锁流量最大化。

隔离用例 `n=2048 b=32`：

| 线程 | 1 | 2 | 4 | 8 | 16 | 24 | 32 | 40 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Round 2 | 0.98x | 1.86x | 3.54x | 6.74x | 12.09x | 14.08x | 11.98x | 8.42x |
| Round 3 | 1.00x | 1.87x | 3.59x | 6.99x | 12.56x | **15.70x** | **14.97x** | **9.90x** |

峰值 14.08 → 15.70x（+12%），32 线程 +25%，40 线程 +18%。相对 Round 0 的 11.32x 累计 +39%。

全集聚合（`xeon_r3_batch_policy.csv`，REPEAT=3，195 个 verifier 全 PASS）：

| 线程 | 8 | 16 | 24 | 32 | 40 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Round 2 | 1.875x | 1.956x | 1.869x | 1.824x | 1.810x |
| Round 3 | 1.899x | 1.926x | 1.886x | 1.817x | 1.802x |

聚合基本持平（16 线程 1.956 → 1.926，在本机 ~5% 噪声内）。收益仍然进不了聚合。

### 关键结论：当前 `b >= 18` 守卫给聚合设了硬上限 2.66x

按 flops 拆解 `benchmark.sh` 的四个 suite（flops 用 `n^3/3` 估算）：

| suite | 用例数 | 串行 / 异步 | 串行占 flops | 该 suite 加速比上限 |
| --- | ---: | ---: | ---: | ---: |
| n512_576 | 14 | 6 / 8 | 44.2% | 2.26x |
| n768 | 11 | 3 / 8 | 27.3% | 3.67x |
| n1024 | 6 | 2 / 4 | 33.3% | 3.00x |
| n1152_small_b | 8 | 4 / 4 | 50.0% | 2.00x |

四个上限的几何平均是 **2.66x**。也就是说只要 `b < 18` 继续走串行路径，
**无论调度器优化到什么程度，聚合 geomean 都不可能超过约 2.66x**。当前 1.926x 已经用掉了
这个上限的 72%。

同时，异步用例本身的 block 数也普遍偏小（n512_576 的异步用例是 B=16/8/4/2/32/24/18/16），
`resolveThreadCount = min(threads, block_count)` 使 B=2 只用 2 线程、B=4 只用 4 线程，
这些用例结构上就用不满 40 核。

因此后续优先级必须调整：**把小 b 带入并行路径是唯一能突破 2.66x 的方向**，
优先于继续打磨大 B 场景的调度器（那部分收益已被聚合结构吞掉）。
`resolveThreadCount` 的上限修正排在其后，且必须谨慎——实测 40 线程比 24 线程差
（9.90x vs 15.70x），单纯放开上限会变差，需要的是"可持续参与者数"的界，
而那个界与 tile 计算时间成正比，在 x86（AVX-512）上拟合的常数会在鲲鹏（NEON/SVE，
task 更长）上偏紧，属于平台拟合陷阱，不能直接写成默认值。

## 2026-08-05 异步阈值由 b>=18 降到 b>=16（Round 4）

背景：Round 3 的结论是 `b < 18` 串行路径把聚合上限锁在 2.66x。本轮先做零代码改动的
阈值扫描，确认 Round 2/3 降低的每 task 开销是否已经让更小的 b 有利可图。

阈值扫描（四 suite 全集，16 线程，REPEAT=3，每档 39/39 verifier PASS）：

| `ASYNC_MIN_B` | 18（原默认） | 16 | 12 | 8 |
| --- | ---: | ---: | ---: | ---: |
| 聚合 geomean | 1.926x | **2.250x** | 2.233x | 1.487x |
| 理论上限 | 2.66x | 4.13x | 5.88x | 无串行用例 |

改动：

- `asyncMinBlockSize()` 默认 18 → 16，`smoke_test.sh` / `benchmark.sh` 的默认值同步对齐。
- 平台方向性论证：aarch64 目标机的向量单元更窄，同一个 b 的 tile task 更长，
  每 task 开销占比更低。因此"在 task 最短的 x86 上有利"的阈值，在鲲鹏上同样有利——
  这个方向的外推是保守的，可以作为默认值。
- 历史记录的"`b >= 16` 实验触发段错误"在当前代码上**未复现**，三档阈值全部 verifier 通过。
  该限制已从 `design.md` 移除。

经验与限制：

- 阈值 12 的理论上限是 5.88x，但实测与 16 持平（2.233x vs 2.250x），说明 b=12/14 用例
  只是被并行化了却几乎没有收益——per-task 开销与 `2*b^3` 的计算量已经同量级。
  b=8 更是明显回退。**继续下调阈值本身不再有效，需要真正改变小 b 的任务粒度或调度开销。**
- 阈值 16 在各线程数下的聚合（`xeon_r4_minb16_sweep.csv`，195/195 PASS）：

| 线程 | 8 | 16 | 24 | 32 | 40 |
| --- | ---: | ---: | ---: | ---: | ---: |
| geomean | **2.293x** | 2.250x | 2.070x | 1.932x | 1.827x |

  **聚合随线程数单调递减。** 判题机不会设置 `COMPILER2026_DAG_THREADS`，
  `resolveThreadCount` 会取 `min(hardware_concurrency, block_count)`，在 40 核机器上
  等于全核，因此实际交付配置拿到的是 1.827x —— 相对原默认在 40 线程的 1.802x 只有 +1.4%。
  也就是说**本轮阈值收益基本被线程过量供给吞掉**，两者是耦合的。
- 隔离测试早已给出同一现象（B=64 在 24 线程 15.70x、40 线程 9.90x）。因此
  `resolveThreadCount` 的过量供给是当前聚合的首要限制，作为 Round 5。

Round 5 的设计约束（避免平台拟合）：可持续参与者数与 tile 计算时间成正比
（`t_task ∝ b^3`），而比例常数取决于机器的锁吞吐。在 x86（AVX-512，task 最短）上拟合的
常数搬到鲲鹏会偏紧、造成供给不足。因此必须先测出"每个 b 对应的最优参与者数"曲线，
再决定用什么形式表达这个界，不能用单点外推。

## 2026-08-05 按 tile 粒度限制参与者数（Round 5）

背景：Round 4 发现聚合随线程数单调递减，而判题机用全核，因此线程过量供给是首要限制。
本轮先测出"每个 (b, B) 的最优参与者数"，再决定界的形式，避免单点外推。

per-case 线程扫描（40 物理核，每点 3 次取最小值，8 个来自四 suite 的真实用例）：

| n | b | B | 2 | 4 | 8 | 16 | 24 | 32 | 40 | 最优 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1152 | 16 | 72 | 1.01x | 1.01x | 1.10x | 1.09x | 1.10x | 1.10x | 1.10x | 32 (1.10x) |
| 1152 | 18 | 64 | 1.45x | 2.56x | **4.20x** | 4.03x | 2.96x | 1.96x | 1.62x | 8 |
| 1152 | 24 | 48 | 1.69x | 3.14x | 5.42x | **8.08x** | 6.41x | 3.51x | 3.57x | 16 |
| 1152 | 32 | 36 | 1.71x | 3.16x | 5.96x | **9.73x** | 9.29x | 6.27x | 6.48x | 16 |
| 1024 | 64 | 16 | 1.88x | 3.53x | 6.09x | **9.28x** | 9.20x | 9.25x | 9.16x | 16 |
| 768 | 96 | 8 | 1.82x | 3.16x | 4.52x | **4.54x** | 4.51x | 4.52x | 4.50x | 16 |
| 512 | 32 | 16 | 1.68x | 2.90x | 4.46x | 5.44x | **5.64x** | 5.61x | 5.55x | 24 |
| 1024 | 32 | 32 | 1.83x | 3.33x | 6.01x | **9.93x** | 8.51x | 6.76x | 6.99x | 16 |

用 40 线程相对每用例最优的损失：b=18 −61%、b=24 −56%、b=32 −33%/−30%；
而 `B <= 16` 的用例因现有 `min(threads, block_count)` 上限已被保护，只有 −1%。

规律：**最优参与者数随 b 近似线性增长**（18→8、24→16、32→24，b>=48 不需要限制），
且与 B 提供的并行度是两个独立的界（同 b=32：B=32 最优 16，B=64 最优 24）。

改动：

- 新增 `participantCapForTile(b)`：`b <= 12 → 4`，否则 `b - 8`。
- `resolveThreadCount` 改为 `min(threads, block_count, participantCapForTile(b))`，
  即"DAG 宽度"和"共享队列可持续吞吐"两个界同时生效。
- `COMPILER2026_DAG_PARTICIPANT_CAP` 可覆盖：正整数强制该值，`off` / `none` 关闭该界
  （per-case 扫描就用它测原始曲线）。默认 auto。
- 补上 `<cstring>` 和 `<limits>` 显式包含——`std::strcmp` / `std::numeric_limits`
  在本机 libc++ 下靠传递包含侥幸编译通过，换到毕昇 libstdc++ 会断。

判题机等效配置（40 核可用、`COMPILER2026_DAG_THREAD_LIST=40`，REPEAT=3，39/39 PASS）：

| | Round 4 | Round 5 |
| --- | ---: | ---: |
| 聚合 geomean | 1.827x | **2.284x** |
| n1024 | — | 2.750x |
| n768 | — | 2.458x |
| n1152_small_b | — | 2.189x |
| n512_576 | — | 1.842x |

+25%。而且 2.284x 已经接近"任意线程数下的历史最好值"（Round 4 在 8 线程的 2.293x），
说明运行时现在能自动为每个用例选对参与者数，不再依赖外部覆盖。

平台限制（必须记录）：`b - 8` 这个常数编码的是本机锁吞吐与 tile 计算时间的比值。
鲲鹏向量单元更窄，同一个 b 的 task 更长，真实可持续参与者数更高，因此该默认值在目标机上
会**供给不足**——损失吞吐但不影响正确性。换平台前应先用
`COMPILER2026_DAG_PARTICIPANT_CAP=off` 重测曲线再定值。

距离目标的差距：阈值 16 下的理论上限是 4.13x，当前 2.284x 用掉 55%。
按 suite 看：n1152_small_b 已达其上限的 82%（剩余串行占比 37.5% 主导），
n768 45%、n1024 46%、n512_576 56%。因此后续两条路线：
（1）小 b（`b < 16`）真正并行化——这是唯一能抬高上限的方向；
（2）中等规模用例的异步加速比仍有约一倍空间。
注意 `b=16` 的用例即使走异步也只有 1.10x，说明小 b 的问题不是阈值而是每 task 开销
与 `2*b^3` 计算量同量级，必须改变任务粒度或调度开销本身。

## 2026-08-05 更正 Round 5 的 b=16 测量并把阈值降到 12（Round 6）

### 更正：Round 5 表格里 b=16 那一行测的是串行路径

`percase_sweep.sh` 是在"把 `asyncMinBlockSize()` 默认改成 16"之后、但**同步到远端之前**运行的，
远端构建出的仍是阈值 18 的代码；脚本又直接调用 `contestant_app` 而没有传
`COMPILER2026_ASYNC_MIN_B`，于是 `b=16 < 18` 全程走串行。因此 Round 5 记录的
"b=16 最优 32 线程 1.10x"以及由此得出的"b=16 即使异步也只有 1.10x，小 b 的问题是每 task
开销"这一推论**是错的**。`b >= 18` 的其余各行是真异步路径，结论有效，
`participantCapForTile` 的拟合不受影响。

教训：直接调用已构建的二进制来做扫描时，必须显式传入所有会改变默认行为的环境变量，
或确认二进制来自当前源码。profile 输出为空（连 `[compiler2026_profile]` 行都没有）
就是"根本没走异步路径"的可靠信号，本轮正是靠它发现问题。

用当前源码重建后的真实小 b 曲线（`COMPILER2026_ASYNC_MIN_B=8`
+ `COMPILER2026_DAG_PARTICIPANT_CAP=off`，每点 3 次取最小值，5/5 verifier PASS）：

| n | b | B | 2 | 4 | 8 | 16 | 24 | 32 | 40 | 最优 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1152 | 16 | 72 | 1.36x | 2.46x | **3.76x** | 2.60x | 1.98x | 1.39x | 1.15x | 8 |
| 1024 | 16 | 64 | 1.60x | 2.60x | **4.04x** | 2.86x | 2.40x | 1.58x | 1.40x | 8 |
| 1152 | 12 | 96 | 1.01x | **1.65x** | 1.59x | 1.14x | 0.94x | 0.62x | 0.51x | 4 |
| 1152 | 9 | 128 | 0.68x | **0.75x** | 0.65x | 0.48x | 0.34x | 0.26x | 0.21x | 4 |
| 1152 | 8 | 144 | **0.54x** | 0.54x | 0.46x | 0.34x | 0.25x | 0.19x | 0.16x | 4 |

两个结果：

- `b=16` 在 8 个参与者下能到 **3.76x–4.04x**，远好于此前误测的 1.10x。
- `participantCapForTile` 的预测与实测最优**完全吻合**：b=16 → 8（实测 8），
  b=12 → 4（实测 4）。这是对 Round 5 那个界的独立验证。
- 真正的交叉点在 9 与 12 之间：b=12 有 1.65x 正收益，b=9 是 0.75x、b=8 是 0.54x 回退。

### 改动：默认阈值 16 → 12

阈值对照（cap 开启，40 核判题机等效配置，REPEAT=3，每档 39/39 PASS）：

| `ASYNC_MIN_B` | 16 | **12** | 10 |
| --- | ---: | ---: | ---: |
| 聚合 geomean | 2.284x | **2.337x** | 2.318x |
| n1152_small_b | 2.189x | 2.353x | 2.298x |

Round 4 测阈值 12 时没有 participant cap，b=12 用了 16 个参与者只有 1.14x，所以当时看不到
收益；现在 cap 给它 4 个参与者，收益就出现了。**这说明阈值和参与者数是耦合的，
必须一起调。** 聚合上 +2.3% 落在本机 ~5% 噪声带内，但 per-case 机制证据是确定的
（b=12 单用例 1.65x vs 串行 1.0x），且 b=10 更差，故采纳 12。

同步更新 `smoke_test.sh` / `benchmark.sh` 默认值和 `design.md`；并补齐 design.md 中
Round 2/3/5 的机制描述（此前仍写着已被移除的静态批量钳制规则）。

### 已标定的性能模型与目标可达性

用 `speedup_case = eff × min(cores, B, cap(b))`（`b < 阈值` 或 `B < 2` 记为 1.0）
按 flops 加权合成 suite speedup，效率取实测均值 **0.46**（10 个用例在各自最优点的
实测效率区间 0.35–0.58）：

| 场景 | 预测 geomean |
| --- | ---: |
| 当前（eff 0.46, cap=b−8） | 2.42x |
| eff 0.60, cap=b−8 | 2.79x |
| eff 0.80, cap=b−8 | 3.19x |
| eff 0.46, cap 翻倍 | 2.76x |
| eff 0.80, cap 翻倍 | 3.51x |
| 理想 eff 1.0, cap 翻倍 | 3.80x |

实测 2.337x 对预测 2.42x，误差 4%，模型可用于判断方向。结论：

- **在 tile 级任务 + panel 局部 DAG 的结构下，这批公开用例的聚合上界约 2.4x，已经到顶。**
- 要达到"当前基准 200%"（1.796x × 2 = 3.59x），需要调度效率接近理想值**且**可持续参与者数
  翻倍。也就是说这是结构性问题，不是参数调优问题。
- 效率损失的来源已部分量化（隔离用例 b=32 B=64、24 线程）：`exec_ms` 1586ms 相对
  serial 1430ms 多出 11%，是并行下 tile 在核间迁移的 cache 代价；`worker_idle_ms` 274ms
  约占核时 11%；其余为 barrier 尾部和主线程提交时间。
- 因此后续唯一有意义的方向是降低每 task 调度成本本身（per-worker 队列 / work stealing，
  它同时提高效率与可持续参与者数），以及重新评估跨 panel DAG——它能消除高 B 时的
  每 panel barrier（B=72 时有 72 次 barrier），而此前否证它的两个前提
  （提交瓶颈、线程过量供给）都已在 Round 2/5 中消除。

## 2026-08-05 跨 panel 二次否证、批量上限放开（Round 7）

### 跨 panel DAG 二次否证（更强的证据）

Round 1 否证跨 panel 时，可以辩解说"提交瓶颈和线程过量供给掩盖了它的收益"。
Round 2/5 已消除这两个前提，因此本轮重测（40 核判题机等效配置，REPEAT=3，各 39/39 PASS）：

| 配置 | geomean |
| --- | ---: |
| 默认 panel 局部 | 2.337x |
| 跨 panel + sync-cholesky，`DAG_MAX_LIVE=0` | 2.341x |
| 跨 panel + sync-cholesky，`DAG_MAX_LIVE=2048` | 1.835x |

仍然没有收益。`DAG_MAX_LIVE=2048` 明显更差，因为该路径每次 submit 都强制 flush，
等于关闭了 Round 2 的提交批量——这也反过来验证了提交批量的价值。

**根因解释（这条结论应终止跨 panel 方向）**：participant cap 把参与者数限制在 24 以内，
而 panel 局部 DAG 的 ready 宽度实测有 1134（Round 2 profile）。**可用并行度比可持续参与者数
高出一到两个数量级**，因此任何"增加可用并行度"的结构性改动都不可能有收益。
跨 panel 只有在调度器可持续参与者数超过 panel 局部 ready 宽度（约 `B^2/2`）时才有意义，
这个条件在本工作负载下极为遥远。

### 判别实验：锁流量不是主导损失

隔离用例 `n=1024 b=32`（cap 关闭，每点 3 次取最小值）扫 `COMPILER2026_TASK_BATCH`：

| TASK_BATCH | T=16 | T=24 | T=32 | T=40 |
| ---: | ---: | ---: | ---: | ---: |
| 2 | 9.14x | 7.24x | 6.73x | 6.66x |
| 4 | 9.22x | 7.62x | 6.80x | 6.84x |
| 8 | 9.22x | 7.51x | 6.75x | 6.91x |
| 16 | **9.94x** | 7.62x | 6.96x | 7.20x |

批量从 2 提到 16 使锁流量降低 8 倍，但只买到约 9%，而且曲线形状不变（仍在 16 线程见顶后下降）。
**因此锁流量既不是效率损失的主因，也不是参与者数上限的成因。** 效率损失的已知构成
（隔离用例、24 线程）：`exec_ms` 比 serial 多 11%（并行下 tile 在核间迁移的 cache 代价）、
`worker_idle_ms` 约占核时 11%、其余为 barrier 尾部与互斥量阻塞（后者不被现有 profile 计入，
本机 `perf` 对该内核不可用，未能进一步细分）。

注：`perf` 在这台机器上对内核 5.15.0-181 不可用（缺 `linux-tools-5.15.0-181-generic`），
且 `perf_event_paranoid=4`。后续若要细分互斥量阻塞时间，需要装对应内核包或在 runtime 里
加一个默认关闭的锁等待计时。

### 改动：批量上限放开

- `kMaxTaskBatch` 16 → 32；批量粒度档位 `b <= 64` 8 → 16、`b <= 128` 4 → 8。
- 聚合验证（40 核，REPEAT=3，各 39/39 PASS）：`TASK_BATCH=8` 2.336x、`16` 2.368x、
  `32` 2.369x。16 已饱和，故取 16 而不是 32（更小的栈数组、更低的延迟风险）。

### 最终默认配置

无任何环境覆盖、40 核可用（判题机等效）：

| suite | speedup |
| --- | ---: |
| n1024 | 2.683x |
| n768 | 2.526x |
| n1152_small_b | 2.409x |
| n512_576 | 1.913x |
| **聚合 geomean** | **2.363x** |

39/39 verifier PASS，IR 断言正常（`submit_deps=2, wait_calls=1, trsm_calls=2, madd_calls=2`）。
相对本轮工作开始时的 1.796x 累计 **+31.6%**。

已达到 Round 6 标定模型给出的结构性上界（预测 2.42x，实测 2.363x）。

## 2026-08-05 参与者数上限加入 panel 宽度依赖（Round 8）

背景：Round 5 的 `participantCapForTile(b)` 只依赖 b。但实测里 `b=32` 在 `B=32` 时最优是 16
（9.93x），在 `B=64` 时最优是 24（15.70x）——同一个 b 的最优参与者数还依赖 block 数。
当时的规则 `min(cap(b), B)` 对 `b=32, B=32` 给出 24，比最优的 16 差 14%。

第一版公式（**回退，已修正**）：

```cpp
shaped = max( min(blocks, 16), min(cap(b), blocks/2) );
```

聚合 2.363x → **2.192x（−7.2%）**，四个 suite 全部变差。原因是 `min(blocks,16)` 这个下限
被错误地套在了粒度上限之上：`b=16, B=72` 因此拿到 16 个参与者，而实测最优是 8
（3.76x vs 2.60x，差 31%）。下限的本意只是抬高"平均 panel 宽度"这个界，不应抬高粒度界。

修正版：

```cpp
width  = max( min(blocks, 16), blocks/2 );   // 下限只作用于宽度界
shaped = min( cap(b), width );
```

三个界的物理含义：

- block 数：DAG 的 panel 数上界。
- 粒度界 `cap(b)`：共享队列在该 tile 计算时间下的可持续参与者数。
- 平均 panel 宽度：panel 序号从 `B` 递减到 1，超过约一半 block 数的参与者在后半程空闲。
  这正是区分 `b=32,B=32`（最优 16）与 `b=32,B=64`（最优 24）的量——只用粒度界会对两者
  都预测 24。
- 宽度界的下限 `min(B,16)`：block 数少时前几个 panel 仍有 `B(B-1)/2` 个任务，
  半宽过于保守（`b=64,B=16` 最优是 16 而非 8）。

修正版对全部 11 个实测最优点都吻合。聚合验证（40 核，REPEAT=3，39/39 PASS）：

| | Round 7 | Round 8 第一版 | Round 8 修正版 |
| --- | ---: | ---: | ---: |
| 聚合 geomean | 2.363x | 2.192x | **2.416x** |
| n1024 | 2.683x | 2.625x | 2.713x |
| n768 | 2.526x | 2.372x | 2.597x |
| n1152_small_b | 2.409x | 2.108x | 2.482x |
| n512_576 | 1.913x | 1.762x | 1.950x |

+2.2%。该幅度落在本机 ~5% 噪声带内，但**四个 suite 同向改善**比单一聚合数字更有说服力，
且机制明确（修正 `b=32, B=32` 区域），故保留。

教训：用多点拟合出的公式，必须逐点回代检查——第一版虽然"拟合了 11 个点"，但 `max`/`min`
的嵌套次序写错，实际在 `b=16, B=72` 处输出 16 而非拟合目标 8。回代一遍就能发现。

## 2026-08-05 40 核 AArch64 云桌面私有 SSH 接入

经组委会确认允许使用自有组网方式建立远程接入后，对 CourseGrading 云桌面完成环境探测：

- openEuler 22.03 LTS、aarch64、40 个在线 CPU、单线程/核、单 NUMA 节点、75 GiB 内存；
- 容器无 `/dev/net/tun`，但出站 HTTPS 正常且已有 `/usr/sbin/sshd`；
- Tailscale userspace networking 可以登录 Tailnet，当前链路可能经 DERP 中继；
- 内置 Tailscale SSH 能建立 TCP 连接，但服务端不返回 SSH banner；
- `userspace tailscaled -> Tailnet TCP Serve :22 -> 127.0.0.1:2222 -> 独立 sshd`
  已用标准 OpenSSH 端到端验证联通。

接入状态、host key、authorized keys 和日志全部保存在 `/mnt/cgshare/tailscale-cloud`，不进入
仓库；`sshd` 仅监听 loopback，关闭密码/PAM/交互式认证，Tailnet 侧只使用私有 Serve，不启用
Funnel。仓库新增无个人 IP/密钥内容的 `scripts/cloud_desktop_ssh.sh` 和
`scripts/sync_to_cloud_desktop.sh`，为后续远程构建、全量 verifier 和性能评测自动化提供固定入口。

该结果只证明远程通道成立。基础镜像仍默认缺少 CMake、Ninja 和 BiSheng；在同源、同输入、
同计时口径的 AArch64 测试与正式 judge 结果对齐之前，不把该环境当作正式性能证据。

环境边界同步更新：原 4-vCPU AArch64 VM 已退役；`43.142.45.204:6000` 继续只用于 Ubuntu
x86_64 多核调度实验。后续 AArch64 构建、verifier 和目标相关性实验统一进入 40 核云桌面。
本次只更新远程接入/测评环境，不改性能实现、调参默认值或历史性能文档。

## 2026-08-05 判题指标改为 per-case 计量；b=8 回退被否证为代码布局噪声（Round 9）

### 最重要的一条：此前一直在优化错误的指标

`submission/scripts/benchmark.sh` 报的是**每个 suite 的总时间比**，而技术方案 PDF 里判题用的是
**等权重 per-case 几何平均**：

```text
speedup_i   = T0_i / T_i
geo_speedup = (prod speedup_i) ** (1 / N)
score       = 0.4 * functional + 0.6 * (100 * geo_speedup / m_ideal)
```

总时间比是 flops 加权的，被最大的用例支配；等权重几何平均里 `n=128` 和 `n=2048` 权重完全相同。
两者在同一份数据上差别很大（150 用例，40 物理核，PASSES=3 取 per-case 最小值，150/150 PASS）：

| 指标 | 值 |
| --- | ---: |
| 等权重 per-case geomean（**判题指标**） | **3.123x** |
| 总时间比（`benchmark.sh` 报的） | 3.425x |
| `m_ideal=32` 下的总分 | 45.86 |

新增计量链（本地工具，不进提交包）：

- `tools/percase_harness/main_percase.cpp`：逐用例计时，写 per-case CSV。除计时外与官方
  `main.cpp` 调用方式完全一致。
- `scripts/percase_bench.sh`：串行参考 + contestant 各跑 `PASSES` 遍整进程，取 per-case 最小值。
  与 `benchmark.sh` 不同，任何 verifier 非 PASS 直接失败——没有正确性证据的性能数字不是结果。
- `scripts/score_judge.py`：按判题公式算分，并按 speedup 区间和 `b` 分桶给出 log 贡献占比。
- `scripts/merge_percase.py`：多遍取 per-case 最小值。

`PASSES>1, REPEAT=1` 是忠实配置：判题一个进程跑完 150 个用例，runtime 在 resolved thread count
变化时会重建 worker pool，这笔开销落在该用例的**首次**调用上；进程内 repeat 会复用 pool 把它藏掉。

证据：`docs/benchmark_results/r9_percase_baseline.csv`。

### 等权重下的杠杆分布

反事实（直接改 CSV 里的 speedup 再算分），说明钱在哪：

| 假设 | geomean | 总分 | Δ分 |
| --- | ---: | ---: | ---: |
| 现状 | 3.123x | 45.86 | — |
| 27 个 <1.0 的用例全部抬到 1.0 | 3.217x | 46.03 | +0.18 |
| `b<=10` 的 22 个用例抬到 2x | 3.488x | 46.54 | +0.69 |
| `b<=10` 的 22 个用例抬到 3x | 3.702x | 46.94 | +1.09 |
| **所有** 150 个用例各 +20% | 3.748x | 47.03 | +1.17 |

两条结论：

- `m_ideal=32` 下每 1x geomean 只值 **1.875 分**，分数对性能极不敏感；但排名看的是 geomean 本身，
  所以仍然只优化 geomean。
- **22 个 `b<=10` 用例（14.7% 的用例数）停在 ~0.95x，其杠杆约等于全部用例一起 +20%。**
  等权重下这是最大的单块空间，对应 roadmap §3 的 range task 方向。

### b<12 的 0.93x 不是调度问题，也不是 Pass 的问题

先用 `COMPILER2026_ASYNC_MIN_B=999` 让 150 个用例全部走 Pass 的串行路径（一个 task 都不提交）：
geomean **0.9916x**，其中 `b<12` 桶 **0.93x**（`docs/benchmark_results/r9_allserial.csv`）。
所以这 22 个用例在任何调度发生之前就已经亏了 ~7%。

`scripts/serial_overhead_probe.sh` 用三个二进制隔离原因，只测 22 个 `b<=10` 用例（4.8s 串行工作量）：

- `pristine`：一次 clang++ 直接编译，即串行参考。
- `roundtrip`：同样走 `-emit-llvm -c` → `opt -passes=no-op-module` → `clang++ <bc>`，**不加 pass**。
- `pass`：真实 contestant 构建，用 `ASYNC_MIN_B=999` 强制走串行路径。

| | rt/pristine | pass/roundtrip |
| --- | ---: | ---: |
| geomean（22 例） | 0.9959x | 0.9329x |
| `b=8`（17 例） | — | **0.9066x** |
| `b=9`（2 例） | — | 1.0327x |
| `b=10`（3 例） | — | 1.0254x |

`.bc` 绕路本身不要钱（0.9959x，`rt/pristine` 按构造应为 1.00，用它当噪声底）。惩罚**只出现在
`b=8`**，`b=9`/`b=10` 完全没有——这个不连续性排除了"per-call 开销按 1/b³ 平滑增长"的解释。

沿着"Pass 让串行路径代码变差"这条线做了两次结构改动，两次都**没有**移动这个数字：

1. `noinline` 加在 async clone 上。第一版无效，因为属性加在 `CloneFunctionInto` **之前**——
   该函数会把源函数的 attribute list 覆盖到目标上，加在前面的会被静默丢弃。
   **同一个 bug 也一直在丢 `compiler2026.skip`**（`isBlockCholesky` 用它防重复变换）。
   移到 clone 之后才生效：`async_impl` 重新变成独立符号，`block_cholesky` 从 0x979 缩回 0x3f2。
   b=8 仍为 0.9305x。
2. 把串行体也 outline 出去，`block_cholesky` 只剩 dispatch（`buildDispatchBody`）。
   b=8 仍为 0.8978x。

最后用 `scripts/layout_sensitivity_probe.sh` 直接量代码布局的影响：**同一个不加 pass 的程序**编译 5 次，
唯一区别是链在 kernels 之前的 padding 字节数（0/16/32/48/80），把之后所有 text 地址推移。
只测 17 个 `b=8` 用例，17/17 PASS：

| padding | 0 | 16 | 32 | 48 | 80 |
| --- | ---: | ---: | ---: | ---: | ---: |
| geomean（pad0/变体） | 1.000x | 0.9564x | 0.9929x | 0.9661x | **0.9009x** |

- 单用例最大 spread **1.182x**，中位 spread 1.135x，整体 geomean spread 1.110x。
- `__official_madd_impl` 只有 275 字节，`b=8` 用例要调用它 ~n³/6b³ 次，因此它落在哪个
  64 字节偏移上直接决定几个百分点。

**结论：`b=8` 那 ~10% 完全落在本机代码布局噪声内，不是 Pass 的性质，也不该对它做优化。**
证据：`docs/benchmark_results/r9_layout_sensitivity.csv`。

推论（后续所有实验都受这条约束）：**本机任何 `b=8` 的测量都带 ±10% 的布局分量，
单次构建的 b=8 结论不可信**，必须跨 padding 或跨构建取分布。

### 两处 Pass 改动:已回退,不入库

沿"Pass 让串行路径变差"这条线做过两处改动,都**没有**移动 b=8 的数字:

- 属性改到 `CloneFunctionInto` 之后设置（修复 `compiler2026.skip` 一直被静默丢弃)。
- 两条路径都 outline,`block_cholesky` 只剩 dispatch。

全量验证(150 用例,40 核,PASSES=3,150/150 PASS,IR 断言不变):geomean 3.131x 对
3.123x,**+0.26%,落在噪声内,即中性**。

**结论:回退,不保留。** 理由是本轮根本没有找到缺陷——b=8 的回退是布局噪声,
不存在需要修的东西,那就不应该有代码改动。中性改动进提交包只有净风险:

- 判题机是 aarch64 + 毕昇,`noinline` 和代码体积在那边的响应未测。
- 每次 `block_cholesky` 调用多一次 call,以及两份循环体带来的体积增长,都没有收益来抵。
- `compiler2026.skip` 那个 bug 目前是潜在的、无害的:clone 名为
  `compiler2026_serial_impl`/`compiler2026_async_impl`,本来就不匹配 `isBlockCholesky` 的
  `name.find("block_cholesky")`,所以属性丢失当前不改变行为。

回退后重新验证:构建通过,`SPEC_START=97 SPEC_END=100` smoke 串行/contestant 各 4/4 PASS。
`submission/` 相对 Round 8 (`3e2d488`) 零改动。

本轮留下的产出因此全部是**度量设施和结论**(`scripts/`、`tools/`、`docs/`),
不含任何提交包代码改动。属性丢失那个 bug 记录在此,等真正需要 pass 幂等性时再修。

### 经验

- **先确认在优化哪个指标。** 前 8 轮用 flops 加权总时间比调参，而判题是等权重 per-case
  几何平均；等权重下 22 个小用例的杠杆等于全部用例一起 +20%，这个方向此前完全看不到。
- **`CloneFunctionInto` 会覆盖目标函数的属性**，任何 `addFnAttr` 都必须放在它之后。
  这类静默丢失不会报错，只能靠 dump IR 发现——第一次改完没看 IR，白测了一轮。
- **在做"代码变差了"的归因之前，先量代码布局的噪声。** 本机 b=8 的布局噪声就有 ±10%，
  和被归因给 Pass 的量同级。padding 变体是个便宜的判别实验。
- **实测中性的改动不进提交包。** 本轮两处 Pass 改动是顺着一个后来被否证的假设做的；
  假设被否证之后，改动就失去了理由，哪怕它顺手修了一个潜在 bug。判题机是 aarch64 + 毕昇，
  任何未在那里验证过的改动都只贡献风险。**先确认存在缺陷，再改代码；否则本轮的产出
  就该只有度量设施和结论。**
- **变体必须交错测量。** 第一版探针按变体分组跑（先全部 pristine，再全部 roundtrip），
  按构造应为 1.00 的 `rt/pristine` 两次分别给出 1.0014x 和 1.0208x——那 2% 是机器漂移，
  分组会让漂移伪装成变体效应。改成每一遍内交错三个变体后噪声底降到 0.4%。

## 2026-08-05 参与者上限是运行时缺陷的补偿，不是负载性质（Round 10 诊断）

本轮只做诊断，不改 runtime/Pass。结论推翻了 Round 6 的"结构性上界 2.4x"。

### 起因

Round 9 指出 22 个 `b<=10` 用例是等权重指标下最大的一块空间，对应 roadmap §3 的 range task。
在动 Pass 之前先量天花板：`tools/coarsen_feasibility/main.cpp` 用**官方算子**和手写 panel barrier
调度重跑同样的分解，只改 madd 粒度：

- `fine`：一个 work unit 一次 madd（等于现在 Pass 发出的粒度）。
- `coarse`：一个 work unit 一整条内层 k 循环（固定 j，写 block 列 j，单元间无写冲突）。

探针刻意偏袒并行侧：spin barrier、常驻 worker、无依赖记账、一个 atomic 做动态负载均衡。
因此它的数字是**上界**，不是预测。串行参考是 `contest::block_cholesky` 原样，
并行结果与串行**逐位相同**（`max_abs_diff=0.000e+00`，5/5 用例）。

### 主要发现：不是粒度问题，是调度器扩展性问题

用**生产环境的串行时间**做统一 T0 后对比（40 物理核）：

| n | b | B | 生产 | 探针@40 线程 | 差距 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1152 | 8 | 144 | 0.865x | 6.214x | 7.2x |
| 2048 | 8 | 256 | 0.992x | 9.600x | 9.7x |
| 1152 | 12 | 96 | 1.746x | 6.491x | 3.7x |
| 1152 | 16 | 72 | 3.344x | 8.993x | 2.7x |
| 1792 | 32 | 56 | 12.270x | 17.987x | 1.5x |

**即使在生产已经走异步路径的 b=12/16/32 上，一个朴素 barrier 调度也快 1.5–3.7 倍。**

线程扫描（`docs/benchmark_results/r10_coarsen_thread_sweep.csv`）说明差距的来源：

| n=1152 b=16 | 4 | 8 | 16 | 24 | 40 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 探针 fine | 3.09x | 5.63x | 6.42x | 8.47x | **8.99x** |
| Round 6 实测生产 | 2.46x | 3.76x | 2.60x | 1.98x | 1.15x |

探针**单调上升到 40 线程**；生产在 8 个参与者见顶后一路跌到 1.15x。同样的负载、同样的算子、
同样的机器。所以那条下降曲线是**运行时的性质，不是负载的性质**。

推论：**Round 5/8 拟合的 `participantCapForTile` 是在补偿运行时的扩展性缺陷。**
Round 6 由此得到的"tile 级任务 + panel 局部 DAG 下聚合上界约 2.4x，已经到顶"是错的——
上界是被单全局队列（mutex + condvar + 单锁 ready queue）压出来的，不是工作负载给的。
Round 7 "锁流量不是主导损失"的判别实验只扫了 `TASK_BATCH`（批量出队），
没有换掉共享队列本身，所以没能看到这一层。

同一线程数下的对比说明两层损失都存在：`n=1152 b=16`、8 线程，探针 fine 5.63x 对生产 3.76x
（约 1.5x 是调度机制本身），再从 8 线程放开到 40 线程又拿到 1.6x（这部分是 cap 挡掉的）。

### 次要发现：最优粒度确实随 b 变化

| | b=8 (n=2048) | b=12 | b=16 | b=32 |
| --- | ---: | ---: | ---: | ---: |
| fine @40 | 3.81x | 6.49x | 8.99x | **17.99x** |
| coarse @40 | **9.60x** | 6.49x | 6.43x | 11.43x |

- `b=8`：必须 coarsen。fine 在 8 线程后就平了（3.6→3.8x），coarse 一直涨到 9.6x。
- `b=12`：两者相同。
- `b>=16`：fine 更好，coarse 反而伤性能——按 j 粗化只留下 `B-p-1` 个单元，
  并行度和负载均衡都变差。

所以粒度必须按 `b`（和线程数）选，这正是 roadmap §3 说的自适应粒度；但它是**第二**优先级，
第一优先级是让调度器能用满核。

### 反事实：把差距按桶外推到 150 个用例

假设按测到的桶差距外推，`n<=256` 不动（探针在那里更差：`n=256 b=8` 探针 0.635x 对生产 0.893x）：

| 假设 | geomean | 总分 |
| --- | ---: | ---: |
| 现状 | 3.131x | 45.87 |
| 只拿到测到差距的**一半** | 5.427x | 50.18 |
| 拿到全部测到的差距 | 7.451x | 53.97 |

这是外推，不是实测；但即使只拿到一半，也是 45.9 → 50.2。相比之下 Round 5–8 四轮参数调优
总共只从 1.796x 走到 3.13x，而且其中相当一部分是在给这个缺陷打补丁。

### 经验

- **在给一条曲线拟合经验公式之前，先确认曲线的形状是负载给的还是自己实现给的。**
  `participantCapForTile` 被拟合了两轮（Round 5 单变量、Round 8 加 panel 宽度），
  逐点都吻合——但拟合的是自己运行时的缺陷曲线。一个 200 行的手写调度探针就能证伪它。
- **判别实验要换掉被怀疑的部件，而不是调它的参数。** Round 7 扫 `TASK_BATCH` 得出
  "锁流量不是主因"，但批量出队仍然走同一把锁、同一个 condvar；真正的对照是**没有**共享队列。
- **上界要用独立实现测，不要用自己的模型算。** Round 6 用 `eff × min(cores, B, cap(b))` 合成出
  2.42x 的"上界"并据此判定方向已尽；模型里的 `cap(b)` 本身就来自缺陷，于是模型把缺陷
  当成了物理规律。

### 机制定位：单线程建图 + 共享队列，worker 在饿死和抢锁之间来回

隔离用例 `n=1152 b=16 B=72`（cap 关闭，`COMPILER2026_DAG_PROFILE=1`，串行参考 0.186s）：

| | 8 参与者 | 40 参与者 |
| --- | ---: | ---: |
| wall | 0.0593s | **0.1724s** |
| tasks / dag_nodes | 64752 | 64752 |
| main_tasks | 833 | 138 |
| dequeue_batches | 11397 | 52996 |
| 每次出队拿到的 task | 5.61 | **1.22** |
| queue_ms（所有 task 排队时间之和） | 3777 | 9117 |
| exec_ms（所有 task 执行时间之和） | 264 | 358 |
| worker_idle_ms | 57 | **2268** |

读法：

- **task 数是固定的 64752，而且每个 task 都建一个 DAG node。** 建图和提交全部由主线程串行完成：
  `main_tasks` 只有 138–833，说明主线程几乎不执行 task，时间都花在提交上。
- 40 参与者时**批量出队塌到 1.22**（8 参与者时是 5.61）。不是批量策略变了，而是 ready queue
  长期接近空——worker 抽干的速度快于主线程建图的速度，于是每个 task 都变成一次完整的
  mutex 往返。
- 同时 `worker_idle_ms` 从 57ms 涨到 2268ms（36 个 worker 平均各空等 63ms，而 wall 只有 172ms，
  约 37% 时间在空等）。**饿死和抢锁同时出现**，这是"生产者带宽不足 + 单锁"的典型signature。
- `exec_ms` 从 264ms 涨到 358ms（+36%）：这部分是真实的 tile 跨核迁移 cache 代价，
  与 Round 6 的观察一致，但它只是次要项。
- 注意 `exec_ms=264ms` 对串行 186ms：即使只算 task 执行本身，聚合核时也已经是串行的 1.4 倍。

所以瓶颈是**每个 madd 一个 task 节点**这件事本身：主线程要为 64752 个 madd 各做一次
`alloc context + 5 次 store + 取锁 + 建 DAG node + 两次 producer 查表 + notify`。
探针完全没有这一层（work 由下标算术并行发现），所以它能线性扩展到 40 线程。

结论：**下一步必须减少调度单元数量**，而不是继续调 batch、cap 或线程数。
探针里"b>=16 时 coarse 不如 fine"的结论不适用于生产——探针的提交成本是零，
生产的提交成本正是瓶颈，所以生产侧粗化同时省掉提交成本和锁流量。

## 2026-08-05 相位屏障 + 提交深度随参与者数缩放 + 池不再重建（Round 11）

Round 10 定位到瓶颈是"每个 madd 一个 DAG node、由单线程串行建图"。本轮三处改动,
每一处都由测量驱动、单独验证。

### 改动 1:panel 内相位屏障,madd 不再需要依赖边（Pass）

原来 trsm 发布 output key、每个 madd 解析两个 key。改为在 trsm 循环出口插一个 `wait`,
之后 trsm 和 madd 都走无依赖的 `submit`。正确性依据:panel 内所有 trsm 互相独立
(都只依赖主线程刚同步执行完的 cholesky),所有 trsm 完成后每个 madd 写不同的 block (k,j),
因此 madd 之间也互相独立。代价是放弃 trsm/madd 重叠——Round 10 的探针带着同样的屏障
仍快 1.5–9.7 倍,说明重叠的价值低于表达它所需的记账成本。

IR 断言随之变化:`ir_submits=2 (plain=2 deps=0) ir_wait_calls=2`。
`percase_bench.sh` 的断言改为对 plain+deps 求和,并新增"wait 数不能为 0"的检查
(0 个 wait 意味着 madd 会与 trsm 竞争)。

单独效果(`n=1152 b=16`,cap off):T=8 3.76x→4.21x,T=16 2.60x→4.10x。
但 T>=24 仍然塌陷,所以依赖记账不是全部原因。

### 改动 2:提交暂存深度必须随参与者数缩放（runtime）

profile 直接给出了原因(`n=1152 b=16`,已去掉依赖边):

| | 8 参与者 | 40 参与者 |
| --- | ---: | ---: |
| wall | 0.0472s | 0.1550s |
| 每次出队拿到的 task | 6.90 | **1.00** |
| 平均 ready 宽度 | 382 | **15.8** |
| worker_idle_ms | 38 | 4317 |
| flushes | 4116 | 4116 |

提交侧每攒够 `task_batch_size_`(16)才发布一次,而等待的 worker 有 36 个。**16 永远
喂不满 36 个消费者**,队列建立不起深度,每个 task 退化成一次完整的 mutex 往返
(`dequeue_batches` 恰好等于 task 数,64752 次)。这在 `chooseBatchCount` 里还是结构性的:
它在 `available <= participants*2` 时返回 1,而 16 个 task 的一次发布在 36 参与者下
永远跨不过这条线。

因此把"提交暂存深度"和"出队批量上限"拆成两个量:后者仍受 `kMaxTaskBatch=32` 限制
(出队用固定大小栈数组),前者改为 `participants * 8`,上限 4096,存储从
`std::array` 改为 `std::vector`。

单独效果(`n=1152 b=16`,cap off):T=16 4.10x→**5.92x**,T=40 1.28x→3.43x。峰值从
8 参与者移到 16–20。

### 改动 3:重新标定参与者上限,并去掉两个 block 数界（runtime）

在 cap off 下重新扫参(`scripts/participant_sweep.sh`,201 个数据点,每点都过 verifier,
证据 `docs/benchmark_results/r11_participant_sweep.csv`)。曲线形状变了:b>=24 从"见顶后塌陷"
变成"饱和"。

| | T=4 | T=8 | T=12 | T=16 | T=20 | T=24 | T=40 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1152 b=12 | 2.16 | 3.44 | **3.85** | 3.38 | 3.05 | 2.83 | 2.11 |
| 1152 b=16 | 2.75 | 4.63 | 5.81 | 5.80 | **6.25** | 5.50 | 4.74 |
| 1152 b=24 | 3.11 | 5.59 | 7.33 | 8.71 | **9.44** | 9.11 | 9.13 |
| 1152 b=32 | 3.53 | 6.35 | 8.82 | 10.70 | **11.54** | 11.49 | 11.49 |
| 1152 b=64 | 3.58 | 6.37 | 8.66 | 10.59 | 10.53 | **10.63** | 10.57 |
| 1152 b=128 | 3.37 | 5.45 | **5.71** | 5.70 | 5.72 | 5.69 | 5.71 |

新规则只保留一个界:

```cpp
cap(b) = 12   if b <= 12
         20   if b <= 16
         无上限 otherwise
participants = max(1, min(threads, cap(b)))
```

**两个 block 数界被实测否证后删除**,不是为了简化:

- `min(threads, blocks)`:一个 panel 有多达 `blocks*(blocks-1)/2` 个 madd,可用并行度
  不受 panel 数限制。`1152 b=128` 只有 9 个 block,却从 8 参与者的 5.45x 涨到 12 参与者的
  5.71x;`640 b=64`(10 个 block)峰值在 16。
- Round 8 的平均 panel 宽度界 `max(min(blocks,16), blocks/2)`:它对 `1152 b=32` 预测 18,
  实测最优 20;它存在的唯一目的是解释一条已经不再出现的下降曲线。

b>=24 直接用满线程数,相对该用例自身最优最多亏 3.3%(b=24,9.13x 对 9.44x),
换来的是常数不再编码本机的锁吞吐。小用例也不需要"保护":`n=128 b=16`(8 个 block)
在 40 参与者下是 1.05x,而本轮之前是 0.258x。

### 改动 4:worker 池只增不减,多余 worker 挂起（runtime）

前三处改完后全量跑出现**新的回退**:`n=128 b=32` 从 0.485x 掉到 **0.159x**,
`n=192 b=24` 掉到 0.395x。原因是 cap 让线程数在 20(b<=16)和 40(b>=24)之间来回,
而 `runtime_begin` 原来在线程数变化时**重建整个池**;一次重建约 2.4ms,
而 `n=128 b=32` 的串行时间只有 446µs。

这个回退只有 per-case 口径能看到:单用例扫参里同一个 shape 是 3.16x,因为池已经热了。
`benchmark.sh` 的总时间比也看不到——它被大用例支配。

改为:池按进程内最大需求建一次,只增不减;`resetForCall` 设置本次调用的
`active_workers_`,序号超出的 worker 挂在**独立的 `park_cv_`** 上。用独立 cv 是必须的:
若挂起的 worker 也等在 `work_cv_` 上,它可能吃掉一次本该唤醒活跃 worker 的 `notify_one`,
让 ready 的工作继续睡着。

效果:`n=128 b=32` 0.159x → **1.079x**,`n=192 b=24` 0.395x → 1.05x 量级。

### 全量验证

150 用例,40 物理核,PASSES=3 REPEAT=1,`INPUT_BIN` 复用,150/150 verifier PASS:

| | geomean | 总分 | <0.95x 的用例数 | 最差用例 |
| --- | ---: | ---: | ---: | ---: |
| Round 8/9 默认 | 3.123x | 45.86 | 19 | 0.252x |
| 本轮(改动 1–3) | 3.629x | 46.80 | 5 | 0.159x |
| 本轮(全部四处) | **3.643x** | **46.83** | **3** | 0.107x |

按 b 分桶:`12<=b<32` 2.92x→3.16x,`32<=b<128` 5.29x→**6.34x**,`b>=128` 2.85x→3.70x。
最大单点收益:`n=2048 b=64` 11.20x→**18.03x**,`n=1536 b=12` 2.23x→4.64x,
`n=128 b=32` 0.49x→1.08x。累计 **+16.7%**。

证据:`docs/benchmark_results/r11_pool_gating.csv`(以及未加池门控的对照
`r11_barrier_staging.csv`)。

### 已知残留成本(未修,已量化)

`n=128 b=16`(idx 1)是全程**第一个**走异步路径的用例,因此它独自承担了整个进程的
worker 池构造(~3.4ms,而它的串行时间是 0.365ms),测得 0.107x。同一 shape 在池已热时是
1.05x。完全消除它只值 +1.5% geomean,且干净的做法是在库加载时预建池——把真实的
进程级初始化移出计时区。本轮不做,先记录代价。

### 经验

- **一个共享队列的生产者暂存深度必须和消费者数量成比例,否则加核必然变慢。**
  这是本轮最可复用的一条:16 个 task 一次发布喂 36 个 worker,队列永远建立不起深度,
  批量出队塌到 1,加核只增加锁竞争。判断信号是 `dequeue_batches ≈ task 数` 且
  `worker_idle` 同时很高——饿死和抢锁同时出现。
- **删掉一个界,要有实测反例,不能只因为"新规则更简单"。** 两个 block 数界各有一个
  明确反例(`b=128 B=9` 在 12 参与者下更快;`b=32 B=36` 最优 20 而公式给 18)。
- **per-case 口径抓到了单用例扫参和总时间比都看不见的回退。** 池重建的代价只在
  "一个进程内连续跑不同 b、每个用例只计首次调用"时才暴露。这正是判题的形状,
  也是 `percase_bench.sh` 用 `PASSES>1, REPEAT=1` 而不是进程内 repeat 的原因。
- **挂起的线程不要和活跃线程共用一个 condition variable。** 否则 `notify_one`
  可能被挂起线程吃掉,造成 ready 工作无人唤醒的延迟。

## 2026-08-05 两个被否证的假设：串行 trsm、更深的提交暂存（Round 12 诊断）

本轮**没有代码改动入库**。两个假设都实测否证，按"确认进步再保存修改"的约定回退，
只保留结论。

### 剖析:现在的瓶颈不再是锁流量

`n=1152 b=16`,20 参与者(该 b 的新最优点),串行 0.2012s:

| 指标 | 值 | 读法 |
| --- | ---: | --- |
| 每次出队拿到的 task | 4.45 | 锁流量已摊薄,不再是 1.0 |
| 平均/最大 ready 宽度 | 203 / 414 | 队列有深度了 |
| exec_ms | 268 | 相对串行 201ms 膨胀 **1.33x** |
| worker_idle_ms | 253.7(19 worker) | 每 worker 13.4ms,占 wall 的 **42%** |
| wait_calls / wait_ms | 143 / 17.4ms | 143 = 2×72 panel,占 wall 一半 |
| trsm exec_ms | 7.19 | 只占总 exec 的 **2.7%** |
| madd exec_ms | 261.1 | |

两项残留损失:worker 空等 42%,以及 task 自身执行时间比串行膨胀 1.33x
(小 tile 在多核间迁移的 cache 代价,与 Round 6/10 一致)。

### 否证 1:串行执行 trsm 以减半屏障数

假设:trsm 只占 2.7% 的工作量却占了一半的屏障(每 panel 两个),把它放回主线程同步执行,
用它的串行时间换掉 72 个屏障。

实测(cap off,每点 3 次取最小值):

| | T=8 | T=16 | T=20 | T=24 | T=40 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1152 b=16 taskized | 4.53x | **6.08x** | 5.82x | 5.41x | 3.05x |
| 1152 b=16 串行 trsm | 4.28x | 3.93x | 5.65x | 5.27x | 3.43x |
| 1152 b=32 taskized | 4.40x | 5.84x | **12.08x** | 11.77x | 9.51x |
| 1152 b=32 串行 trsm | 5.27x | 7.73x | 8.26x | 8.14x | 6.96x |
| 1792 b=32 taskized | 6.77x | 11.87x | 14.28x | 14.62x | **14.96x** |
| 1792 b=32 串行 trsm | 5.91x | 9.35x | 10.56x | 10.20x | 10.04x |
| 1152 b=64 taskized | 6.43x | 10.70x | 12.20x | 12.66x | **14.04x** |
| 1152 b=64 串行 trsm | 4.51x | 6.07x | 6.50x | 6.51x | 7.00x |

6 个用例里 5 个明显变差,`b=64` 直接腰斩(14.04x → 7.00x)。

**错在哪:2.7% 这个比例是从 `b=16` 一个点上读来的,而 trsm 占比按 `1/B` 增长。**
trsm 数约 `B²/2`、madd 数约 `B³/6`,所以 trsm 工作量占比 ≈ `3·c_trsm/(B·c_madd)`。
`b=16` 时 `B=72`,占比 2.7%;`b=64` 时 `B=18`,占比高一个数量级,串行化它等于把
一大块工作搬回单线程。**用一个点的比例去推所有 b 是本轮最主要的方法论错误。**

### 否证 2:更深的提交暂存

假设:Round 11 把暂存深度设为 `participants*8` 拿到了大幅改善,继续加深应该继续改善。

实测 `SUBMIT_STAGE_PER_THREAD ∈ {4, 8, 16, 32}`(cap off,每点 3 次取最小值):

| | T=16 | T=20 | T=24 | T=40 |
| --- | ---: | ---: | ---: | ---: |
| 1152 b=16, st=4 | 5.99x | 5.81x | 5.12x | 3.92x |
| 1152 b=16, st=8 | 6.11x | 5.99x | 5.47x | 4.31x |
| 1152 b=16, st=16 | 6.09x | 6.06x | 5.53x | 4.59x |
| 1152 b=16, st=32 | 5.75x | 5.44x | 5.26x | 4.47x |
| 1792 b=32, st=4 | 12.54x | 14.84x | 15.03x | 15.17x |
| 1792 b=32, st=32 | 12.31x | 14.11x | 14.09x | 14.39x |

**4 到 32 之间全部落在 ±5% 内,已经饱和。** Round 11 的收益来自"从 16(固定值)变成
随参与者数缩放",而不是"更深"——一旦深度超过参与者数一个小倍数,再加深没有意义。
默认 8 保留,环境变量旋钮回退(默认不变的旋钮不是改进,不入库)。

### 顺带发现:本机单次测量可能偏 2 倍

否证 1 的第一轮里 `1152 b=32` 在 T=16 读到 5.84x,而 Round 11 的扫参读到 10.70x。
加到每点 3 次取最小值后稳定在 10.4–11.1x——**5.84x 是单次测量的离群值,偏了近 2 倍。**
这和 Round 9 的布局噪声是两件不同的事(那个是构建间差异,这个是同一二进制的运行间差异)。
结论:此后任何用于决策的点都必须多次取最小值;`participant_sweep.sh` 的单次模式只适合
看趋势,不适合定阈值。

### 经验

- **从单个数据点读出的比例,不能外推到整条参数轴。** trsm 占比随 `1/B` 变化,
  在 `b=16` 上量到的 2.7% 对 `b=64` 完全不成立。要外推,先确认这个量在轴上的标度关系。
- **"某方向有收益"不等于"该方向越多越好"。** 提交暂存深度的收益在超过参与者数几倍后
  完全饱和;Round 11 的真实机制是"与消费者数量成比例",不是"更深更好"。
- **负面结果同样要留证据。** 这两个假设都合理、都便宜,但都错;记录下来可以阻止
  以后重复尝试,尤其是串行 trsm 这种"看起来显然能减少同步"的想法。

## 2026-08-05 range task：粒度交给运行时决定（Round 13）

本轮落地 roadmap §3 的 range task,是本次会话最大的一步:geomean **3.643x → 4.31x**。

### 设计：通用 parallel-for，运行时不理解算子

新增运行时 API:

```c
void compiler2026_runtime_submit_range(void (*fn)(void*, int, int), void *ctx, int count);
```

运行时把 `[0, count)` 切成若干块,每块作为一个 task 调用 `fn(ctx, begin, end)`。
**运行时不解释 ctx,也不解释下标含义**,只决定怎么切——因此这仍然是通用任务调度,
不是把算子语义写进运行时。

Pass 侧生成 `compiler2026_task_madd_range`,把内层 k 循环变成一个 range task:

```c
for (int t = begin; t < end; ++t)
    madd(A0 + t*b*n, B0, C0 + t*b*n, b, n);
```

步长来自源循环里算子自己的实参形态:`madd(&L[k*n+i], &L[j*n+i], &L[k*n+j], b, n)`,
k 每步 +b,所以第一、第三个指针每步前进 `b*n` 个 double,第二个不变。
**发出的 madd 调用、实参和顺序与原循环完全一致,只是分组方式变了。**

三个量都由 `tile_dag.v1` 声明的 row-major `L` 语义恢复,不需要分析 PHI:

```text
elements = (B0 - L) / sizeof(double) = j*n + i     (i < n)
j        = elements / n
A0 = B0 = &L[j*n + i]        (k = j 时 A 就等于 B)
C0      = &L[j*n + j]
count   = (n - j) / b        (k = j, j+b, ... < n)
```

**CFG 不做手术**:只删掉内层循环里的 madd 调用,循环本身留给后续 -O2 的 loop deletion
处理(此时它已无副作用)。如果 loop deletion 没生效,代价只是一个空循环,不会错。
若 `B0` 不是循环不变量或拿不到 preheader,就回退到原来的"每个 madd 一个 task"。

### 粒度规则：每个 task 恒定工作量

`rangeChunkLength` 用 `目标 flops / (2b³)` 决定一个 chunk 包含多少个 madd:

| b | 2b³ | chunk(目标 2e5) | 效果 |
| ---: | ---: | ---: | --- |
| 8 | 1024 | 195 | 整条 k 循环 = coarse |
| 12 | 3456 | 57 | |
| 16 | 8192 | 24 | |
| 32 | 65536 | 3 | |
| >=64 | >=524288 | 1 | 每个 madd 一个 task = fine |

这条规则同时复现了 Round 10 探针的两端(b=8 要 coarse:3.81x 对 9.60x;b>=32 要 fine:
17.99x 对 11.43x),中间自然插值。**粒度不再由 Pass 写死,而是运行时按 b 决定。**

### 实测（cap off，repeat=3，每点过 verifier）

| 用例 | Round 11 最优 | 本轮最优 | 提升 |
| --- | ---: | ---: | ---: |
| 1152 b=8 | 0.93x(串行路径) | **3.62x** | — |
| 2048 b=8 | 0.99x(串行路径) | **7.28x** | — |
| 1024 b=8 | 0.92x(串行路径) | **4.20x** | — |
| 1152 b=9 | 0.97x(串行路径) | **3.94x** | — |
| 1280 b=10 | 1.00x(串行路径) | **5.45x** | — |
| 1152 b=12 | 3.85x | **5.81x** | 1.51x |
| 1152 b=16 | 6.25x | **7.50x** | 1.20x |
| 1152 b=32 | 11.54x | 11.52x | 1.00x |
| 1792 b=32 | 15.25x | 15.40x | 1.01x |
| 1152 b=64 | 10.63x | **14.35x** | 1.35x |
| 1152 b=128 | 5.72x | **8.19x** | 1.43x |

`b>=64` 的收益(chunk=1,粒度没变)来自**省掉了每个 madd 一次 context 分配和一次 submit**:
一个 j 只提交一次,主线程不再是关键路径。证据 `docs/benchmark_results/r13_range_task_sweep.csv`。

### 随之调整的两个默认值

- 参与者上限重新标定为 **b<48 → 24,否则不限**。粒度变大后最优参与者数普遍升到 24。
  代价是跨界的两个用例最多亏约 7%(`b=32 B=56` 想要 40,`b=128 B=9` 想要 24),
  小于本机单次测量的离散度。
- 异步阈值 `ASYNC_MIN_B` 从 **12 降到 8**。当一个 task 就是一个 madd 时,b=8 的 1024 flops
  付不起一次队列往返;range task 在 b=8 把整条 k 循环合成一个 task 后,同样的用例从
  0.93x/0.99x 变成 3.62x/7.28x,交叉点已经落到本题最小的 b 以下。

### 首个异步用例会独自承担全部预热成本（打地鼠，已根治）

全量跑暴露一个反复出现的坑:**进程里第一个走异步路径的用例,要独自付掉整个运行时的
一次性预热开销**,而它的串行时间可能只有几百微秒。

| 配置 | idx 0 (128 b=8) | idx 6 (192 b=16) | 全局最差 |
| --- | ---: | ---: | ---: |
| range task | **0.089x** | 1.527x | 0.089x |
| + 小用例走串行 | 0.956x | **0.309x** | 0.309x |
| + 加载时建线程池 | 0.899x | **0.611x** | 0.611x |
| + 加载时跑一次空并行域 | 0.914x | 0.920x | **0.914x** |

读法:把小用例挡到串行路径,账单只是**搬到了下一个异步用例**(idx 0 → idx 6);
在加载时建线程池只解决了一半(0.309x → 0.611x),因为线程创建之外还有 arena 第一个 1MB
chunk、队列 vector 首次分配、每个 worker 的首次 futex 唤醒和栈缺页。
在加载时跑一次**丢弃的空并行域**把整条路径走热,才真正根治:全局最差从 0.089x 变成 0.914x。

线程池是进程级资源,在库加载时构造它是它本来该待的地方(OpenMP 运行时也是这样);
惰性构造只是把它记到了第一个计时区里。惰性构造仍然保留作为兜底,初始化没跑到只会更慢、
不会更错。

### 全量验证

150 用例,40 物理核,PASSES=3 REPEAT=1,150/150 verifier PASS,
IR 断言 `ir_submits=1 (plain=1 deps=0) ir_wait_calls=2 ir_trsm_calls=2 ir_madd_calls=3`:

| | geomean | 总分 | 最差用例 | <0.95x |
| --- | ---: | ---: | ---: | ---: |
| Round 8/9 起点 | 3.123x | 45.86 | 0.252x | 19 |
| Round 11 | 3.643x | 46.83 | 0.107x | 3 |
| **Round 13** | **4.308x** | **48.08** | **0.914x** | 5(全部 0.91–0.98) |

分桶:`b<12` 0.99x→**1.95x**,`12<=b<32` 3.16x→**3.9x**,`32<=b<128` 6.34x→**6.4x**,
`b>=128` 3.70x→**3.7x**。本次会话累计 **+38%**。

### 测量能力已经成为瓶颈（重要）

同一份配置连跑两遍全量:geomean **4.017x 和 4.247x**,差 **5.7%**。
因此**全量聚合数字无法分辨 3% 以内的改动**。本轮几个小改动都落在这个带内,
只能靠"逐用例看机制"来判断:

- 判别方法:把用例分成"该改动可能影响的"和"不可能影响的"两组。
  加 work guard 那次,不可能被影响的 143 个用例整体从 4.748x 掉到 4.564x(0.961x),
  而 idx 70/71/72(都是 n=768)同向掉到 0.66–0.75x——这是主机瞬时扰动,不是改动的效果。
  真正由改动引起的只有 7 个目标用例,全部按预期变成 ~1.0x。
- 结论:**此后小幅改动必须用"配对逐用例 + 分组对照"判断,不能只看聚合 geomean。**

### 经验

- **把"切多大"交给运行时,把"切什么"留给 Pass。** range task 让运行时按 `b` 选粒度,
  一条 `目标flops/(2b³)` 规则同时覆盖 b=8 要整条循环、b>=64 要单个 madd 两个极端,
  不需要 Pass 猜,也不需要运行时懂算子。
- **改循环结构时优先"删副作用 + 让后续 pass 清理",而不是自己动 CFG。** 只删 madd 调用、
  把空循环留给 loop deletion,避免了 preheader/exit PHI 的手术,错了也只是慢。
- **一次性预热成本会附着到第一个进入该路径的用例上,并且会随着策略变化"搬家"。**
  只要它还在计时区内,挡掉一个小用例就只是换一个受害者;必须在计时区之外把整条路径走热。
- **当改动幅度接近测量噪声时,聚合指标失去分辨力。** 要么降噪,要么改用配对逐用例对照——
  本轮如果只看聚合,会误判 work guard 是回退(实际是 +1.25%)。

## 2026-08-05 range task 预算 200000 → 50000 flops（Round 14）

`COMPILER2026_RANGE_TASK_FLOPS` 扫参:6000 / 12500 / 25000 / 50000 / 100000 / 200000 /
400000 / 800000,每点 3 次取最小值,`n=1152`、`b` 从 8 到 128、参与者 16/24/40。

默认参与者数(b<48 → 24)下,50000 对 200000:

| b | 8 | 12 | 16 | 24 | 32 | 64 | 128 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 200000 | 3.63x | 5.96x | 7.52x | 9.07x | 11.09x | 12.66x | **7.95x** |
| 50000 | **3.94x** | **6.12x** | **7.80x** | **9.84x** | **11.46x** | 12.66x | 7.65x |

7 个用例里 6 个改善或持平(+3%~+8%),只有 `b=128` 亏 4%。**往更小走会翻转**:
6000 时小 tile 崩掉(`b=8` 3.35x、`b=12` 4.30x),因为 chunk 携带的工作量已经付不起它
自己那次队列往返。所以最优点是"每个 task 约 5µs 工作量"这个量级,不是"越细越好"。

全量验证(150 用例,150/150 PASS,连跑两遍):geomean **4.632x / 4.227x**,总分 48.68 / 47.92。
两遍之间差 9.6%——再次说明聚合数字在这台机器上分辨不了这个量级的改动,采纳依据是
逐用例、多档、同向的扫参证据,不是聚合数字。

### 经验

- **"某方向有收益"仍然不等于"越多越好"(第二次踩到)。** Round 12 在提交暂存深度上验证过
  一次饱和,本轮在 range chunk 上验证了一个**真正的极值点**:两侧都变差。区别在于暂存深度
  是"够了就行",chunk 大小是"太大则并行度不足、太小则摊不平开销"。遇到新旋钮应先扫两侧,
  确认是饱和型还是极值型。
