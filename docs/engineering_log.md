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
