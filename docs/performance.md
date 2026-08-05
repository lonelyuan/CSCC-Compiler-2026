# 性能记录

本文件记录当前 IR-level Pass + runtime 的性能,按判题的**等权重 per-case 几何平均**口径测量。
自 Round 15 起,**首选平台是与官方同 ISA / 同 OS / 同工具链的 aarch64 机器**(见下);
40 物理核 x86_64 Xeon 调试机的数据保留作对照,因为历史各轮常量都是在那台机器上标定的。
早期 4 vCPU aarch64 VM 的 suite 总时间比数据口径不同,仅存档在文末。

## 计量口径（重要）

判题用的是**等权重 per-case 几何平均**,不是各 suite 的总时间比:

```text
speedup_i   = T0_i / T_i
geo_speedup = (prod speedup_i) ** (1 / N)
score       = 0.4 * functional + 0.6 * (100 * geo_speedup / m_ideal)
```

总时间比是 flops 加权的,被最大的用例支配;等权重下 `n=128` 和 `n=2048` 权重完全相同。
同一份数据两者差别很大(150 用例:等权重 4.25x,总时间比 7.64x)。因此性能结论一律以
`scripts/percase_bench.sh` + `scripts/score_judge.py` 的 per-case 口径为准。
`submission/scripts/benchmark.sh` 的 suite 总时间比只适合看大用例的相对变化。

## 当前有效结果（aarch64，最接近官方平台）

平台:HiSilicon aarch64 40 核 / 单 NUMA / 2.9 GHz 定频(boost disabled)/ L3 32 MiB,
openEuler 22.03 LTS,BiSheng Enterprise 3.2.0.1.B004 clang 15.0.4。
与官方平台同 ISA、同 OS、同编译器;经 tailscale 隧道接入,入口见 `scripts/cg.sh`。

方法:官方 150 个公开用例,`PASSES=3 REPEAT=1`,每用例取 3 遍最小值,两侧全过 verifier。
该主机有 **2 GiB 内存 cgroup 硬上限**,而 per-case harness 要驻留 2.46 GB,
因此用 `scripts/percase_bench_chunked.sh` 分 5 段跑再合并打分(切段不改变单用例计时)。

| 轮次 | per-case geomean | 总分(m_ideal=32) | 最差用例 | 证据 |
| --- | ---: | ---: | ---: | --- |
| Round 14 代码在 aarch64 复测 | 4.253x | 47.97 | 0.490x | `r15_cg_aarch64_baseline.csv` |
| **Round 15**(chunk 预算 → 200000) | **4.370x** | **48.19** | 0.492x | `r15_cg_flops200k.csv` |

两轮均 150/150 verifier PASS,IR 断言
`ir_submits=1 (plain=1 deps=0) ir_wait_calls=2 ir_trsm_calls=2 ir_madd_calls=3`。

Round 15 的聚合 +2.7% 落在本机 5.7–9.6% 的运行间噪声带内,采纳依据是配对分组对照:
按"两档预算算出的 chunk 长度是否不同"分组,**受影响的 80 个用例 3.383x → 3.553x(+5.0%)**,
**对照的 70 个用例 5.524x → 5.535x(+0.2%,即无主机漂移)**,且效果随 b 单调
(`b=24` +23.9%、`b=18` +13.5%、`b=28` +16.2%)。

分桶(等权重下用例**数量**才是权重),与 Xeon 同一份代码对照:

| 桶 | 用例数 | Xeon | aarch64 (Round 14 常量) | arm/xeon | aarch64 (Round 15) |
| --- | ---: | ---: | ---: | ---: | ---: |
| `b < 12` | 22 | 2.10x | 1.94x | **0.92** | 1.91x |
| `12 <= b < 32` | 39 | 4.22x | 3.60x | **0.85** | **3.97x** |
| `32 <= b < 128` | 61 | 6.11x | 6.56x | 1.07 | 6.59x |
| `b >= 128` | 28 | 3.64x | 3.87x | 1.06 | 3.91x |

**聚合分数与 Xeon 在同一噪声带内,但来源换了位置:小 tile 变差、大 tile 变好。**
原因是这台机器的串行基准只要 Xeon 的 0.55 倍时间(官方 `madd` 在此定频无降频的核上
向量化良好),于是所有以"每 task 多少 flops"表达的常量都偏小。Round 15 修正了其中最大的一项:
range chunk 预算 50000 → 200000,`12<=b<32` 桶因此 +10.3%。
证据 `r15_cg_range_flops_sweep.csv`。

### 剩余空间与优先级（等权重口径，已量化）

| 杠杆 | 估计收益 | 状态 |
| --- | ---: | --- |
| chunk 预算 → 200000 | +0.22 分 | **已采纳**(Round 15) |
| 参与者上限按 b 单调重标定 | ~+0.13 分 | 已定位未验证,见 `r15_cg_participant_sweep_200k.csv` |
| 8 个 <0.95x 用例全救到 1.00x | +0.14 分 | 已定位(串行交叉点偏低) |

**三项加起来约 0.5 分,而 `score 50` 需要 geomean 5.33x,是当前的 1.22 倍。**
常量重标定不足以换档;真正的空间在结构上(跨 panel 依赖恢复、去掉两次相位屏障)。
常量重标定的作用是让结构性改动在**正确的工作点**上被评估。

## 对照结果（40 物理核 x86_64 Xeon 调试机）

平台:40 物理核 / 80 逻辑核 x86_64 调试机(Xeon Gold 5218R ×2,Ubuntu 22.04,LLVM 17),
`taskset -c 0-39` 绑定物理核。**这不是官方性能平台**;Round 8–14 的全部常量都在这台机器上
标定,Round 15 已证明其中至少 range chunk 预算是平台专属拟合。

方法:官方 150 个公开用例(`cases/preliminary_public_150.txt`),整进程跑 3 遍、
每用例取 3 遍中的最小值(`PASSES=3 REPEAT=1`)。`REPEAT=1` 是忠实配置——判题一个进程跑完
150 个用例,任何 per-case 首次调用开销都必须计入。串行参考与 contestant 使用同一 harness、
同一输入,两侧输出全部过 verifier;任何用例非 PASS 直接判失败。

| 轮次 | 主要改动 | per-case geomean | 总分(m_ideal=32) | 最差用例 |
| --- | --- | ---: | ---: | ---: |
| Round 8 | tile DAG + participant cap | 3.123x | 45.86 | 0.252x |
| Round 11 | 相位屏障 + 提交暂存随参与者缩放 + 池只增不减 | 3.643x | 46.83 | 0.107x |
| Round 13 | range task + 阈值降到 8 + 加载时预热 | 4.308x | 48.08 | 0.914x |
| Round 14 | range task 预算 200000 → 50000 flops | 见下 | | |

150/150 verifier PASS,IR 断言
`ir_submits=1 (plain=1 deps=0) ir_wait_calls=2 ir_trsm_calls=2 ir_madd_calls=3`。

按 block size 分桶(Round 13,等权重下用例**数量**才是权重):

| 桶 | 用例数 | geomean |
| --- | ---: | ---: |
| `b < 12` | 22 | 1.95x |
| `12 <= b < 32` | 39 | 3.9x |
| `32 <= b < 128` | 61 | 6.4x |
| `b >= 128` | 28 | 3.7x |

单点最高:`n=2048 b=64` 18.03x,`n=1792 b=32` 15.40x,`n=1152 b=64` 14.35x。

证据 CSV:

```text
docs/benchmark_results/r9_percase_baseline.csv     Round 8 起点
docs/benchmark_results/r11_pool_gating.csv         Round 11
docs/benchmark_results/r13_warmup.csv              Round 13
docs/benchmark_results/r11_participant_sweep.csv   参与者数扫参(201 点)
docs/benchmark_results/r13_range_task_sweep.csv    range task 扫参
docs/benchmark_results/r9_layout_sensitivity.csv   代码布局噪声
```

## 测量噪声（解读上表时必须知道）

- **运行间噪声**:同一份配置连跑两遍全量,geomean 分别为 4.017x 和 4.247x,差 **5.7%**。
  因此上表中 3% 以内的差异没有分辨力,小改动必须用配对逐用例对照(把用例分成"该改动可能
  影响的"和"不可能影响的"两组)来判断。
- **构建间噪声**:`b=8` 用例带约 ±10% 的代码布局分量。同一个**不加 pass** 的程序,
  只改链接 padding 字节数,17 个 `b=8` 用例的 geomean 就在 0.90x–1.00x 之间摆动
  (`r9_layout_sensitivity.csv`)。任何 `b=8` 结论都不能来自单次构建。

## 历史结果（4 vCPU VM，口径不同，仅作存档）

早期在 openEuler aarch64 4 vCPU VM 上用 suite 总时间比记录的结果(约 1.657x)与上表口径
不同,且平台差异过大,不作为当前性能记录。

## 2026-07-17 annotation 关键路径实验（非正式本机结果）

函数级 `tile_dag.v1` annotation 已能在 Clang/LLVM 20 `-O2` 后保留，并使跨 panel
路径在 GEP 被折叠为 PHI 时仍通过 pointer-difference 恢复坐标。本机 x86_64、4 线程、
无 profile、每组重复 3 次；两个跨 panel 候选使用 sync-cholesky 和
`DAG_MAX_LIVE=2048`，默认 guard 使用 panel-local / `DAG_MAX_LIVE=0`。结果为：

| 路径 | contestant total | speedup geo |
| --- | ---: | ---: |
| annotation 跨 panel，无 priority | 3.571598s | 1.519x |
| annotation 三级 rank，rank-1 自适应 batch | 3.720227s | 1.414x |
| annotation 默认 panel-local guard | 3.079379s | 1.563x |

候选 priority 路径全部通过本地 verifier，但没有超过无 priority 的跨 panel 对照，也没有
超过默认 panel-local guard，因此保持 `COMPILER2026_DAG_CRITICAL_PRIORITY=0`。这些数字来自
本机 LLVM 20，不替代本文件开头的 openEuler aarch64 / BiSheng 15 正式记录；由于当前机器
缺少 VM SSH 密钥，本轮尚无鲲鹏/VM 性能结论。原始 CSV 已归档为
`docs/benchmark_results/local_semantic_crosspanel_guard_repeat3_final.csv`、
`local_semantic_ranked_batch_repeat3_final.csv`、
`local_annotation_default_guard_repeat3_final.csv` 和
`local_semantic_priority_schema_final.csv`。CSV 显式记录了
`pass_cross_panel_dag`、`pass_sync_cholesky`、`dag_critical_priority` 和 IR call-site
计数，避免仅从 label 推断 Pass 配置。

本机复现时使用 `/usr/local/opt/llvm` 的 LLVM 20.1.3、Unix Makefiles；设置
`LLVM_CONFIG`、`CC`、`CXX`、`CLANG`、`OPT`、`LLVM_LINK`、`LLVM_DIS` 和
`CMAKE_GENERATOR="Unix Makefiles"` 后，三组核心命令分别为：

```bash
export LLVM_CONFIG=/usr/local/opt/llvm/bin/llvm-config
export CC=/usr/local/opt/llvm/bin/clang
export CXX=/usr/local/opt/llvm/bin/clang++
export CLANG=/usr/local/opt/llvm/bin/clang++
export OPT=/usr/local/opt/llvm/bin/opt
export LLVM_LINK=/usr/local/opt/llvm/bin/llvm-link
export LLVM_DIS=/usr/local/opt/llvm/bin/llvm-dis
export CMAKE_GENERATOR="Unix Makefiles"

COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1 \
  COMPILER2026_DAG_MAX_LIVE=2048 COMPILER2026_DAG_THREADS=4 \
  COMPILER2026_DAG_CRITICAL_PRIORITY=0 REPEAT=3 \
  LABEL=local_semantic_crosspanel_guard_repeat3_final ./submission/scripts/benchmark.sh

COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1 \
  COMPILER2026_DAG_MAX_LIVE=2048 COMPILER2026_DAG_THREADS=4 \
  COMPILER2026_DAG_CRITICAL_PRIORITY=1 REPEAT=3 \
  LABEL=local_semantic_ranked_batch_repeat3_final ./submission/scripts/benchmark.sh

COMPILER2026_ENABLE_CROSS_PANEL_DAG=0 COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=0 \
  COMPILER2026_DAG_MAX_LIVE=0 COMPILER2026_DAG_THREADS=4 \
  COMPILER2026_DAG_CRITICAL_PRIORITY=0 REPEAT=3 \
  LABEL=local_annotation_default_guard_repeat3_final ./submission/scripts/benchmark.sh

COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1 \
  COMPILER2026_DAG_MAX_LIVE=2048 COMPILER2026_DAG_THREADS=4 \
  COMPILER2026_DAG_CRITICAL_PRIORITY=1 COMPILER2026_DAG_PROFILE=1 REPEAT=1 \
  LABEL=local_semantic_priority_schema_final ./submission/scripts/benchmark.sh
```

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
- 新增 opt-in sync-cholesky cross-panel lowering：`COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1` 时，Pass 保留原始同步 `cholesky` 调用，只在调用前插入 `runtime_wait_key` 等待 diagonal input 的 latest producer；`trsm/madd` 仍进入跨 panel DAG。该路径用于降低 cholesky task 化和完整跨 panel DAG live pressure，仍需 benchmark 证明后才可默认化。
- `runtime_wait_key` 从固定 `10us` 超时轮询改为基于完成通知等待：存在 key waiter 时，ready batch 完成方会通知 `done_cv`，让 sync-cholesky 实验路径更快发现目标 key producer 已完成；默认 panel-local 路径没有 key waiter。
- 新增 opt-in Linux worker pinning：`COMPILER2026_DAG_PIN_WORKERS=1` 时，runtime 会把 worker 绑定到当前进程 affinity mask 中的 CPU，用于真实目标机亲和性实验；默认关闭。
- 提交包新增合法函数级 `tile_dag.v1` annotation；默认 panel-local 路径不变，跨 panel 实验可依据其 row-major `L` 语义通过 pointer difference 恢复被 PHI 折叠的地址坐标。
- 新增默认关闭的 `COMPILER2026_DAG_CRITICAL_PRIORITY=1` 三级 rank 实验。它在本机 LLVM 20 重复测试中未超过无 priority 对照，因此不属于已验证性能提升。

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

`smoke_test.sh` 和 `benchmark.sh` 都会透传 `COMPILER2026_DAG_THREADS`、`COMPILER2026_DAG_PROFILE`、`COMPILER2026_TASK_BATCH`、`COMPILER2026_ASYNC_MIN_B`、`COMPILER2026_ASYNC_MIN_BLOCKS`、`COMPILER2026_DAG_MAX_LIVE`、`COMPILER2026_DAG_PIN_WORKERS`、`COMPILER2026_DAG_CRITICAL_PRIORITY`。因此小范围 verifier smoke 可以直接验证 async 阈值、线程数、profile、batch、live-window、worker pinning 和实验性关键路径 rank 是否真实生效。

`benchmark.sh` 还支持用 `COMPILER2026_DAG_THREAD_LIST=1,2,4` 在一次运行里扫描多个线程数。CSV 的 `threads` 字段区分每条记录，suite 输出目录按 `threads_<count>` 拆分，terminal summary 按线程分组输出 IR 计数、整体 speedup、speedup P50/P95、async decision 和 profile 摘要，避免不同线程数的结果被混合平均。未设置该变量时仍沿用单个 `COMPILER2026_DAG_THREADS`，默认值为 `4`。成功运行后，脚本默认删除 `${LABEL}` 下的大体量 per-suite 输入/输出/profile 目录，保留 `${LABEL}.csv`、`bin/` 和 `ir/`；需要排查单次输出时设置 `COMPILER2026_BENCH_KEEP_ARTIFACTS=1`。

离线参数调优使用 `submission/scripts/tune_params.sh`，不要放进 contestant 计时路径。该包装脚本默认遍历 `COMPILER2026_TUNE_ASYNC_MIN_B_LIST=18,24,32,48` 和 `COMPILER2026_TUNE_TASK_BATCH_LIST=auto,4,8`，并支持显式设置 `COMPILER2026_TUNE_ASYNC_MIN_BLOCKS_LIST`、`COMPILER2026_TUNE_DAG_MAX_LIVE_LIST` 与 `COMPILER2026_TUNE_DAG_PIN_WORKERS_LIST`；后几者默认使用当前单值，避免日常 sweep 组合数膨胀。每个组合调用一次 `benchmark.sh`，再让 `benchmark.sh` 扫 `COMPILER2026_TUNE_THREAD_LIST` 指定的线程列表。所有组合的原始行会追加到 `build/optimization_benchmarks/<label>_aggregate.csv`，terminal 会按 `threads + async_min_b + async_min_blocks + dag_max_live + dag_pin_workers + task_batch` 输出 `tune_summary`。

真实目标机上建议先用 `REPEAT=1` 扫宽参数空间，筛掉明显退化的组合；对 panel-local 默认路径应要求 `dag_missing_deps=0`，对跨 panel 实验路径则要同时查看 `dag_first_touch_deps`，避免把合法的原始输入块 first-touch 误判为异常缺失 producer。候选组合再用 `REPEAT=3` 或更高重复次数复测。当前默认 `b >= 18` / auto batch 8 只代表本 VM 上已归档的稳妥默认，不代表不同 CPU 架构、核心数、cache/内存带宽或 NUMA 拓扑下的最终最优点。

示例：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
COMPILER2026_TUNE_THREAD_LIST=1,2,4,8 \
COMPILER2026_TUNE_ASYNC_MIN_B_LIST=18,24,32,48 \
COMPILER2026_TUNE_ASYNC_MIN_BLOCKS_LIST=2 \
COMPILER2026_TUNE_TASK_BATCH_LIST=auto,4,8 \
COMPILER2026_TUNE_DAG_MAX_LIVE_LIST=0 \
COMPILER2026_TUNE_DAG_PIN_WORKERS_LIST=0,1 \
COMPILER2026_TUNE_LABEL_PREFIX=target_param_sweep \
REPEAT=1 ./submission/scripts/tune_params.sh
```

新增 CSV 字段包括 `task_batch`、`runtime_batch_avg`、`runtime_batch_max`、`async_min_b`、`async_min_blocks`、`dag_max_live`、`dag_pin_workers`、`ir_submit_deps`、`ir_submit_plain`、`ir_wait_calls`、`ir_trsm_calls`、`ir_madd_calls`、`async_decisions`、`async_enabled`、`async_disabled`、`async_disabled_small_b`、`async_disabled_small_blocks`、`async_disabled_threads`、`async_disabled_single_block`、`profile_calls`、`total_tasks`、`main_tasks`、`worker_tasks`、`ready_samples`、`ready_sum`、`ready_avg`、`ready_per_thread`、`dag_nodes`、`dag_edges`、`dag_satisfied_deps`、`dag_missing_deps`、`dag_first_touch_deps`、`dag_initial_ready`、`dag_released`、`dag_release_batches`、`max_dag_release_batch`、`max_dag_pending`、`max_dag_successors`、`max_dag_live`、`queue_ms`、`exec_ms`、`worker_idle_ms`、`main_wait_ms`、`wait_calls`、`wait_ms`、`wait_ready_avg`、`wait_active_avg`、`wait_dag_live_avg`、`max_wait_ready`、`max_wait_active`、`max_wait_dag_live`、`trsm_count`、`madd_count` 等。每个 suite/run 可能包含多个矩阵调用，benchmark 会把同一次运行里的 IR 计数和 profile 行聚合到同一条 CSV 记录。默认不打开 profile 时动态 profile 字段为 `0`，计时 CSV 结构保持一致。benchmark 终端摘要同时输出 IR call site 计数、所有行的 `serial_total`、`contestant_total`、算术平均 speedup、几何平均 speedup、speedup P50/P95 和 profile 模式下的 runtime batch、ready width、DAG release batch、wait span、wait pressure 与依赖状态摘要，避免后续调参只看单 suite 或手算几何平均。

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
docs/benchmark_results/tune_blocks_live_smoke_aggregate.csv
docs/benchmark_results/tune_dims_default_repeat3.csv
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
docs/benchmark_results/dag_first_touch_cross_panel_profile_smoke.csv
docs/benchmark_results/sync_cholesky_default_guard_repeat3_final.csv
docs/benchmark_results/cross_panel_sync_cholesky_profile_final_smoke.csv
docs/benchmark_results/cross_panel_sync_cholesky_live2048_repeat3.csv
docs/benchmark_results/key_wait_notify_default_guard_repeat3.csv
docs/benchmark_results/key_wait_notify_sync_cholesky_live2048_repeat3.csv
docs/benchmark_results/cross_panel_reserve_profile_smoke.csv
docs/benchmark_results/cross_panel_sync_cholesky_reserve_profile_smoke.csv
docs/benchmark_results/drop_completed_producers_profile_smoke.csv
docs/benchmark_results/pin_workers_schema_default_repeat3.csv
docs/benchmark_results/pin_workers_repeat3.csv
docs/benchmark_results/pin_workers_profile_smoke.csv
docs/benchmark_results/cross_panel_fanout_priority_profile_smoke.csv
```

前三个 CSV 来自早期“整函数替换为 runtime 入口”的实验版本。它们的性能更高，但该路线不够符合赛题对 IR 层算子依赖分析的要求，因此不作为当前提交方案。
`profile_csv_smoke.csv`、`ready_queue_profile_csv_smoke.csv`、`dag_profile_counters_smoke.csv`、`panel_dag_cleanup_profile_smoke.csv`、`async_predicate_profile_smoke.csv`、`async_predicate_disabled_smoke.csv`、`async_predicate_threads1_smoke.csv`、`async_decision_profile_smoke.csv`、`async_decision_threads1_smoke.csv`、`benchmark_overall_summary_smoke.csv`、`dag_successor_fanout_smoke.csv`、`gep_operator_key_smoke.csv`、`ir_submit_counts_smoke.csv`、`dag_reserve_structures_smoke.csv`、`panel_task_reserve_smoke.csv`、`queue_reset_lock_smoke.csv`、`main_wait_profile_smoke.csv`、`dag_live_profile_smoke.csv`、`dag_dep_state_smoke.csv`、`ready_width_profile_smoke.csv`、`adaptive_batch_profile_smoke.csv`、`wait_span_profile_smoke.csv`、`ir_wait_count_profile_smoke.csv`、`dag_release_batch_profile_smoke.csv`、`wait_pressure_profile_smoke.csv`、`recursive_gep_key_smoke.csv`、`smoke_env_passthrough_profile_smoke.csv`、`block_coordinate_key_smoke.csv`、`async_min_blocks_profile_smoke.csv`、`async_min_blocks5_profile_smoke.csv`、`thread_sweep_profile_smoke.csv`、`batch8_default_profile_smoke.csv`、`async_min_b24_default_profile_smoke.csv`、`tune_wrapper_profile_smoke_aggregate.csv`、`tune_blocks_live_smoke_aggregate.csv`、`async_min_b18_madd_no_output_default_profile_smoke.csv`、`successor_edge_pool_profile_smoke.csv`、`cross_panel_live2048_profile_final_smoke.csv`、`dag_first_touch_cross_panel_profile_smoke.csv`、`cross_panel_sync_cholesky_profile_final_smoke.csv`、`cross_panel_reserve_profile_smoke.csv`、`cross_panel_sync_cholesky_reserve_profile_smoke.csv` 和 `pin_workers_profile_smoke.csv` 是 profile 数据链验证用的单次重复实验，用于确认 CSV 字段、聚合逻辑、阈值开关、最小 block 数开关、live-window 参数记录、worker pinning 参数记录、线程数开关、线程数扫参 summary 分组、离线调参 aggregate 汇总、smoke/benchmark 环境透传、async decision 原因聚合、整体 summary 输出、DAG successor fanout 统计、DAG release batch 统计、wait 入口 pressure 统计、block key 恢复 smoke 行为、block row/col 恢复到 runtime key 的路径、递归一维 GEP key 恢复路径、IR call site 计数、静态 wait call site 计数、DAG reserve 行为、panel task reserve 估算、runtime reset 加锁后的 profile 链路、main wait 空等统计、DAG live-pressure 统计、依赖解析状态统计、first-touch 输入依赖区分、ready queue 宽度采样统计、自适应 runtime batch 记录、wait span 统计、live-window drain 行为、sync-cholesky key wait 行为、worker pinning 行为和被回退的 cross-panel reserve sizing 行为，不作为正式性能均值。`tune_wrapper_smoke_aggregate.csv` 是调参 wrapper 的早期单组合非 profile smoke；`tune_blocks_live_smoke_aggregate.csv` 是扩展 min-block/live-window sweep 维度后的单组合非 profile smoke；`tune_dims_default_repeat3.csv` 是新增 `dag_max_live` CSV 字段后的默认路径重复验证，`contestant_total=1.764788s` 但 `speedup_geo=1.637x`，不替代当前最佳 geomean 记录；`pin_workers_schema_default_repeat3.csv` 是新增 `dag_pin_workers` CSV 字段后的默认 guard run，`contestant_total=1.790583s`、`speedup_geo=1.633x`，不替代当前最佳；`pin_workers_repeat3.csv` 是 Linux worker pinning opt-in 重复实验，`contestant_total=1.775266s`、`speedup_geo=1.646x`，略好于同轮默认 guard 但仍未超过当前最佳正式记录；`sync_cholesky_default_guard_repeat3_final.csv` 是新增 sync-cholesky 实验接口后的默认路径 guard run，`contestant_total=1.778253s`、`speedup_geo=1.641x`，未超过当前最佳正式记录；`ready_queue_batch8_repeat3.csv` 是早期 task batch 调参对照；`batch8_default_repeat3.csv` 是默认 batch 调整后的正式重复结果；`async_min_b24_default_repeat3.csv` 是上一版默认阈值调整后的正式重复结果；`async_min_b18_madd_no_output_default_repeat3.csv` 是上一版默认阈值和 panel-local output key 优化后的正式重复结果；`successor_edge_pool_repeat3.csv` 是上一版 runtime successor edge pool 优化后的正式重复结果；`live_window_default_repeat3_final.csv` 是当前默认路径正式 geomean 结果。
`cross_panel_opt_in_profile_smoke.csv` 是跨 panel DAG 实验路径的 profile 记录，显示该路径把 panel 内静态 wait 降为外层 DAG 收尾 wait：`ir_wait_calls=1`、`ir_submit_deps=3`，但 4 vCPU VM 上 `speedup_geo=1.499x`、`max_dag_live=6072`、`dag_missing_deps=7595`，低于默认 panel-local 方案。`cross_panel_gate_default_repeat3.csv` 验证新增 gate 后默认路径仍保持 panel-local：`ir_wait_calls=1`、`speedup_geo=1.633x`。
`benchmark_artifact_cleanup_smoke.csv` 是 benchmark artifact cleanup 的单次 smoke 记录，用于证明 CSV 保留和默认清理路径可用，不作为正式性能均值。
`benchmark_percentile_summary_smoke.csv` 是 terminal percentile summary 的单次 smoke 记录，用于证明 suite/overall summary 输出 `speedup_p50` 和 `speedup_p95` 可用；该 CSV 不新增字段，不作为正式性能均值。
`cross_panel_live512_smoke.csv`、`cross_panel_live1024_smoke.csv`、`cross_panel_live2048_smoke.csv` 和 `cross_panel_live4096_smoke.csv` 是 live-window 候选扫参记录；最佳单次 smoke 未超过当前默认 repeat=3，因此不切默认。`cross_panel_live2048_profile_final_smoke.csv` 证明 `COMPILER2026_DAG_MAX_LIVE=2048` 下 `max_dag_live=2050`，但 `contestant_total=0.658224s`、`speedup_geo=1.550x` 仍不足以默认启用。
`dag_first_touch_cross_panel_profile_smoke.csv` 证明跨 panel 路径中 `dag_missing_deps=7595` 全部对应 `dag_first_touch_deps=7595`，即原始输入块 first-touch，而不是同一 DAG 内已知 output producer 丢失。
`cross_panel_sync_cholesky_profile_final_smoke.csv` 验证 sync-cholesky cross-panel 实验路径可用：默认 IR guard 中 `wait_key_refs=0`，实验 IR 中 `wait_key=1`、`cholesky_task_refs=0`、`submit_deps3=1`；profile 中 `max_dag_live=1066`，低于 live-window taskized cross-panel 的约 `2050`，但 `speedup_geo=1.534x`、`contestant_total=0.660494s`，仍低于默认 panel-local 正式结果。`cross_panel_sync_cholesky_live2048_repeat3.csv` 是该路径在 `COMPILER2026_DAG_MAX_LIVE=2048` 下的 repeat=3 正式复测，`speedup_geo=1.587x`、`contestant_total=1.828356s`，仍未超过当前默认 `live_window_default_repeat3_final`，因此不切默认。`key_wait_notify_sync_cholesky_live2048_repeat3.csv` 是 `runtime_wait_key` 改为完成通知后的同配置 repeat=3 结果，`speedup_geo=1.606x`、`contestant_total=1.796239s`，较上一轮 sync-cholesky live-window 复测有改善，但仍未超过默认；`key_wait_notify_default_guard_repeat3.csv` 是默认路径 guard，`contestant_total=1.753659s` 但 `speedup_geo=1.616x`，不替代当前最佳 geomean 记录。
`cross_panel_reserve_profile_smoke.csv` 和 `cross_panel_sync_cholesky_reserve_profile_smoke.csv` 记录了一次已回退的 cross-panel aware reserve sizing 实验：taskized cross-panel 为 `speedup_geo=1.503x`、`contestant_total=0.663732s`，sync-cholesky 为 `speedup_geo=1.532x`、`contestant_total=0.662763s`，均未超过对应旧实验记录，因此没有保留代码改动。
`drop_completed_producers_profile_smoke.csv` 记录了一次已回退的 latest-producer 表收缩实验：producer 完成后如果仍是 latest producer 就从 `latest_producer_` 删除。该策略在 sync-cholesky live-window 路径下得到 `speedup_geo=1.363x`、`contestant_total=0.728694s`，显著低于保留 producer 表的同类实验；同时 `dag_missing_deps=243986`、`dag_first_touch_deps=8012`，说明大量 completed-producer 命中会被改写成 missing producer，破坏当前 profile 诊断语义，因此没有保留代码改动。
`cross_panel_fanout_priority_profile_smoke.csv` 记录了一次已回退的 ready dequeue fanout-priority 实验：该策略只按 runtime 通用 successor fanout 在小窗口内重排 ready task，不读取算子名称；在 `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 COMPILER2026_DAG_MAX_LIVE=2048` 下得到 `speedup_geo=1.452x`、`contestant_total=0.695125s`，低于既有 cross-panel live-window 结果，因此没有保留代码改动。

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

## 2026-08-05 40 物理核平台多轮调度优化（非正式环境，x86_64）

环境：Intel Xeon Gold 5218R ×2，x86_64 / Ubuntu 22.04.5 / glibc 2.35，40 物理核 /
80 逻辑核，2 NUMA node，LLVM 17.0.6（**无毕昇**），`taskset -c 0-39`，governor performance。

**这不是正式性能环境。** 正式成绩仍以鲲鹏 920 / openEuler aarch64 / 毕昇 15 为准；
跨机绝对时间不可比，本节只用于记录同机的相对提升和结构性结论。本文件开头的
`live_window_default_repeat3_final` 仍是唯一的 openEuler aarch64 正式记录。

### 全部 150 个公开用例（默认配置，无任何环境覆盖，40 核可用）

```text
serial_seconds=39.720028994
contestant_seconds=16.130582711
speedup=2.462x
150/150 status=PASS
```

复现命令：

```bash
source /path/to/llvm17.env   # LLVM_CONFIG/CC/CXX/CLANG/OPT/LLVM_LINK/LLVM_DIS
SPEC_START=1 SPEC_END=150 GENERATOR_THREADS=40 VERIFIER_THREADS=40 \
  taskset -c 0-39 ./submission/scripts/smoke_test.sh
```

### 四个 benchmark suite（默认配置，40 核可用，REPEAT=3）

| suite | serial avg | contestant avg | speedup |
| --- | ---: | ---: | ---: |
| `n1024` | 1.236435s | 0.460998s | 2.683x |
| `n768` | 0.946272s | 0.374642s | 2.526x |
| `n1152_small_b` | 1.701613s | 0.707272s | 2.409x |
| `n512_576` | 0.376700s | 0.196945s | 1.913x |

聚合 geomean **2.363x**，39/39 verifier PASS。CSV：`benchmark_results/xeon_r7_final.csv`。

### 多轮进展（同机、同配置、40 核可用）

| 轮次 | 改动 | 聚合 geomean |
| --- | --- | ---: |
| 起点 | — | 1.796x |
| Round 1 | 定向唤醒替代 `notify_all` | 1.802x |
| Round 2 | 批量化 DAG 提交 | 1.810x |
| Round 3 | 移除批量的静态 block 数钳制 | 1.802x |
| Round 4 | 异步阈值 18 → 16 | 1.827x |
| Round 5 | 按 tile 粒度限制参与者数 | 2.284x |
| Round 6 | 异步阈值 16 → 12 | 2.337x |
| Round 7 | 批量上限 8 → 16 | **2.363x** |

累计 **+31.6%**。Round 1–3 在聚合上几乎不动，收益全部体现在隔离用例上
（`n=2048 b=32` 峰值 11.32x → 15.70x，+39%）——因为聚合当时被线程过量供给和
串行路径两重稀释压住，直到 Round 4–6 解除这两个上限，前几轮的调度改进才转化为聚合收益。
这条因果链记录在 `engineering_log.md`。

### 已标定的上界

模型 `speedup_case = eff × min(cores, B, cap(b))`（按 flops 加权合成，效率取实测均值 0.46）
预测 2.42x，实测 2.363x，误差 4%。据此：

- 当前结构（tile 级任务 + panel 局部 DAG）在这批公开用例上的上界约 **2.4x，已经达到**。
- 要再进一步，模型给出的候选：`madd` 算子级子分块（把 `madd(b)` 分解为多个 `madd(b')`
  调用，仍调用官方算子，只改变分块）预测 **3.13x**；对 `b < 12` 做 task 粗化可继续抬高
  `n1152_small_b` 与 `n512_576`。两者都是 Pass 侧较大改动，尚未实施。
- 跨 panel DAG 已被两次否证，原因是可用并行度（ready 宽度 1134）比可持续参与者数（≤24）
  高一到两个数量级，不应继续投入。
