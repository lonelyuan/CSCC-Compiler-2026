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
