# 后续优化路线

> **2026-08-05 Round 27 起,先读本节,再读 Round 15 和其余历史。** Round 15 的结构上限
> 仍是正确的零开销上界,但“做到上限 80%”不能再当作已证明可达:无锁执行和二维复用落地后,
> `b=8` 已在 32 核进入带宽平台,小矩阵则被每 panel 固定延迟主导。相位屏障仍不是首要结构瓶颈,
> 但当前 panel-local 设计在这台机器上的**实测**结果是 7.386x / 53.85 分,不是 12.11x。

## 当前最高优先级：跨过带宽与固定延迟，而不是继续换队列/屏障（Round 17–27）

已采纳三步结构改动:

1. 依赖为空的 phase 使用持久无锁 session、epoch 发布、静态参与者分配和逐 worker 完成标志,
   消除共享 ready queue 的逐 task 锁竞争。
2. Pass 把 MADD 三角域改写成二维 `(row_group,col_group)` range task；小 tile 在同一核上复用
   A/B tile，所有计算仍直接调用官方 `madd`，ABI、运算顺序和 panel barrier 不变。
3. Pass 把每 panel 的 TRSM 行循环改写成一次 range submit；每行仍直接调用官方 `trsm`，但不再
   由单一提交线程逐行分配 context 和提交 task，降低小 tile 的每 panel 固定延迟。

五段全量命令:

```bash
COMPILER2026_TRSM_RANGE=1 LABEL=r27_cg_trsm_range PASSES=3 REPEAT=1 \
  ./scripts/percase_bench_chunked.sh
```

150/150 verifier PASS，证据
`docs/benchmark_results/r27_cg_trsm_range.csv`（以及 `_c1`–`_c5`）:

| 版本 | geomean | 总分 |
| --- | ---: | ---: |
| Round 15 | 4.370x | 48.19 |
| Round 17 持久无锁 phase | 6.123x | 51.48 |
| Round 20 + 二维 tiling | 7.006x | 53.14 |
| **Round 27 + TRSM range outlining** | **7.386x** | **53.85** |

Round 27 分桶:

| 桶 | 用例数 | 实测 | Round 15 零开销上限 | 达成率 |
| --- | ---: | ---: | ---: | ---: |
| `b < 12` | 22 | 7.83x | 36.6x | 21.4% |
| `12 <= b < 32` | 39 | 9.74x | 30.6x | 31.8% |
| `32 <= b < 128` | 61 | 8.13x | 12.9x | 63.0% |
| `b >= 128` | 28 | 3.90x | 4.1x | 95.0% |

TRSM range 的 20 点同轮开/关配对对照成立:`b<12` 为 **1.1400x**、`12<=b<32` 为
**1.0800x**，而机制控制组 `b>=32` 为 **1.0019x**；全组为 **1.0544x**。全量相对 Round 20
的 contestant time 配对为 **1.0482x**，方向和覆盖桶一致。该改动确认降低了小 tile 固定提交
成本，但不足以独自达到 60 分；当前达到 10.667x 还需要全量 **1.444x**。

### 已否证，不要重写

- 两级完成屏障:18 例配对 `0.9997x`。
- 共享原子 counter 屏障:小用例普遍退化；39 次原子写比读取独立完成标志更贵。
- 按 phase task 数收缩参与者:28 例配对 `1.0034x`，噪声。
- 连续静态分配替代循环分配:总体 `0.9988x`；局部缓存收益被负载变化抵消。
- tile packing 后以 `lda=b` 调官方 `madd`:36 点全部正确但总体退化；复制 A/B/C 和 C 回写
  的成本大于 TLB/连续访问收益。
- `b>=128` 仍不投入；Round 27 达成率 95.0%。

### 新的硬证据与路线约束

二维后线程曲线（`n=1152`，所有点 verifier PASS）显示:

| b | T=8 | T=16 | T=24 | T=32 | T=40 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 8 | 6.74x | 10.26x | 12.26x | **13.45x** | **13.55x** |
| 12 | 6.94x | 11.07x | 13.26x | 14.37x | 14.79x |
| 24 | 7.06x | 11.78x | 15.41x | 17.76x | 19.61x |
| 32 | 6.79x | 11.54x | 15.18x | 17.89x | 20.20x |

因此 `b=8` 已经是带宽平台，`b>=24` 仍有核数扩展空间。接下来只有能减少**必需 C 流量**、
显著减少每 panel 固定工作，或同时覆盖 `12<=b<128` 大量用例的改动，才可能提供 1.444x。
矩形 tiling、预取、40 对 38 参与者等最多是个位数百分比，不能当成 60 分主路线。

> **2026-08-05 Round 15 起,本文开头这一节优先于下面所有历史表述。**
> 下面的历史内容把"跨 panel 依赖恢复"当作最大空间。**在目标平台上实测后,这个判断是错的**:
> 相位屏障不是当前的瓶颈,详见下节。

## 第一优先级：小 tile 的执行效率，不是调度结构（Round 15 实测结论）

在 aarch64 目标平台(40 核,详见 `DEVELOPMENT_GUIDE.md` §2.1)上,把**当前设计自己的结构上限**
算清楚之后,结论完全改变。

上限模型(相位屏障设计,零开销、相位内完美并行):以 `b^3` 为单位,
`cholesky = 1/3`、`trsm = 1`、`madd = 2`,`B = n/b`;
每个 panel 的跨度 = `1/3 + ceil(ntrsm/C) * 1 + ceil(nmadd/C) * 2`,C 为核数。

| 桶 | 用例数 | 当前 | 该设计的上限 | 达成率 | 结论 |
| --- | ---: | ---: | ---: | ---: | --- |
| `b < 12` | 22 | 1.91x | 36.6x | **5.2%** | 空间巨大 |
| `12 <= b < 32` | 39 | 3.97x | 30.6x | **13.0%** | 空间巨大 |
| `32 <= b < 128` | 61 | 6.59x | 12.9x | 51.3% | 还有 2x |
| `b >= 128` | 28 | 3.91x | 4.1x | **96.2%** | **已饱和,不要再投入** |

**把这套设计做到自身上限的 80%,geomean = 12.11x,总分 62.71。**
参照:目前第一名 63 分,按 `40 + 1.875 * geo` 反解 geomean ≈ **12.27x**。

也就是说:**不动相位屏障、不做跨 panel DAG,只把小 tile 的执行效率补上,就能到第一名的水平。**
历史各轮反复尝试跨 panel DAG 并三次否证,现在有了解释——它本来就不是瓶颈所在。

### 已经拿到的机制线索

`docs/benchmark_results/r15_cg_participant_sweep_200k.csv`(cap off,chunk 预算 200000):

| b | T=8 | T=16 | T=24 | T=32 | T=40 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 8 | **3.34x** | 3.19x | 3.10x | 2.93x | 2.76x |
| 12 | 3.99x | **4.60x** | 4.59x | 4.40x | 4.32x |
| 32 | 6.36x | 9.73x | 11.47x | **12.39x** | 12.21x |

**`b=8` 加核心是负收益,`b=12` 在 16 核就饱和。** 这是**争用**的特征,不是任务粒度或
调度结构的特征(粒度不够只会让曲线平掉,不会掉头向下)。

### 实验 1（最高优先级）：把"小 tile 加核心变慢"归因到带宽还是锁争用

这两种归因指向完全不同的修法,必须先分清,不要先写代码。

```bash
# perf 已装在目标机上
KNOB=COMPILER2026_DAG_THREADS VALUES="1 2 4 8 16 24 32 40" \
  PIN="COMPILER2026_DAG_PARTICIPANT_CAP=off" CASES="1152:8 1152:16 1152:32" \
  ./scripts/knob_sweep.sh
# 同一批点上收 perf 计数器,关键是每 flop 的访存量随核数是否恒定
perf stat -e cache-misses,cache-references,bus-cycles,instructions,cycles <cmd>
```

判据:

- **带宽饱和** —— 总带宽在 T=8 就到顶,每 flop 的 cache-miss 数不随核数变化。
  `b=8` 的算术强度只有 `1024 flops / 1536 bytes ≈ 0.67 flop/byte`,而本机
  40 核约 900 GFLOP/s 的峰值需要 ~1.3 TB/s 才喂得饱,所以**先验上很可能就是带宽**。
- **锁争用** —— cache-miss/flop 随核数上升,`bus-cycles` 里 atomic/LSE 相关占比上升,
  或者把 ready queue 换成 per-worker deque 后曲线立刻变好。

### 实验 2（若带宽饱和，这是主攻方向）：madd 嵌套的二维 tiling，提高算术强度

**当前 range task 的切法本身就是零复用的。** 一个 task = 固定 `(row, col)` 上一段 k 循环,
于是 `A(row, k)` 和 `B(col, k)` 两条 strip 被完整流过一遍,**没有任何一个 tile 被复用**。

改法:让一个 task = **一个 `(row, col)` 的二维块**共享同一段 k。
例如 4x4 的 `(row, col)` 块:装入 4 个 A tile + 4 个 B tile,做 16 次 madd。
访存 8 个 tile / 16 次 madd,而现在是 32 个 tile / 16 次 madd——**访存量降到 1/4**。

这是纯粹的循环变换(interchange + tiling),属于 Pass 该做的事,而且**完全合规**:
每个 tile 仍然原样调用官方 `madd`,ABI 和语义都不变。
带宽受限下理论上限接近 4x,正好是 `b<12` 和 `12<=b<32` 两桶需要的量级。

配套值得一起试的:**tile packing**。官方 `madd(A, B, C, b, lda)` 接受 `lda`,
所以可以把 tile 复制进连续的 `b x b` 缓冲区、以 `lda = b` 调用官方算子——
ABI 不变,但把 `lda = n` 的跨页跳转变成连续访问,对 TLB 和预取都有利。

### 实验 3（若是锁争用）：把共享 ready queue 换成 per-worker deque + work stealing

Round 10/11 已经证明过共享队列在参与者多时会成为瓶颈(当时的修法是**限制参与者数**,
也就是绕开而不是解决)。如果实验 1 判定为争用,那么正解是让每个 worker 有自己的双端队列、
空了再去偷,把 40 个线程对一把锁的竞争彻底去掉。

### 不要再投入的方向

- **`b >= 128`(28 个用例)**:已经到该设计上限的 96.2%,再优化没有空间。
- **跨 panel DAG / 去掉相位屏障**:上限模型显示屏障不是瓶颈,历史上已三次否证。
  等小 tile 效率补上、`32 <= b < 128` 也接近上限之后再重新评估。
- **继续拟合常量**:Round 15 已量化,所有已知常量错误加起来约 0.5 分。

### 记账用的换算

```text
score = 40 + 1.875 * geo_speedup     (functional 满分, m_ideal = 32)
48.19 分 = 4.37x (当前)
63 分    = 12.27x (第一名)
需要的是 2.8x 的 geomean 提升,只可能来自上面两桶的执行效率
```

## 平台假设（历史，已被 Round 15 的实测平台取代）

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

**计量口径（Round 9 修正，优先于本文其余部分的历史表述）**：判题用的是**等权重 per-case
几何平均**，不是 `benchmark.sh` 报的 flops 加权总时间比。两者在同一份数据上是 3.123x 对 3.425x。
调优必须以 `scripts/percase_bench.sh` + `scripts/score_judge.py` 的 per-case 口径为准；
`benchmark.sh` 的总时间比只适合看大用例的相对变化。等权重下每个用例权重相同，
因此 22 个 `b<=10` 小用例的杠杆约等于全部 150 个用例一起 +20%。

另有一条测量约束：本机 `b=8` 用例带 **±10% 的代码布局噪声**（同一个不加 pass 的程序，
只改链接 padding 字节数，geomean 就在 0.90x–1.00x 之间摆动，见
`docs/benchmark_results/r9_layout_sensitivity.csv`）。任何 `b=8` 结论都不能来自单次构建。

当前 `benchmark.sh` 已支持 `COMPILER2026_DAG_THREAD_LIST=1,2,4` 这类多线程扫参入口，并按线程分组输出 summary；terminal summary 现在同时给出 speedup P50/P95，便于先看分布再决定是否复测。成功运行后默认清理 per-suite 临时大文件，避免长时间 sweep 撑满 VM 磁盘。`tune_params.sh` 在其外层补充 `async_min_b × async_min_blocks × task_batch × dag_max_live × dag_pin_workers` 离线扫参，并汇总 aggregate CSV；新增维度默认是单值，只有显式设置列表才扩展组合空间。真实鲲鹏平台上可直接把线程列表扩展到 `1,2,4,8,16,32,48,64`，再结合 profile CSV 的 ready width、wait pressure 和 queue/exec 时间判断默认阈值、live-window 和 worker pinning 是否需要重设。

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

提交包现已增加版本化函数 annotation `tile_dag.v1`。默认路径仍使用原 GEP 分析；跨 panel
实验可依据 annotation 声明的 row-major `L` 基址语义，用 pointer difference 覆盖 PHI
折叠后的地址形态。第一版三级关键路径 rank 已贯通 Pass/runtime，但本机重复实验没有超过
无 rank 对照，说明在单全局队列上继续调整优先级常量的空间有限；下一步应把 annotation
用于 bounded look-ahead、稠密 tile version 状态或 per-worker queue。

已有第一版跨 panel DAG 实验入口：设置 `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1` 时，Pass 会 taskize `cholesky`，让 `trsm` 依赖 diagonal/latest output producer，让 `madd` 通过 `runtime_submit_deps3` 依赖两个 `trsm` 和自身输出块 previous producer，并把静态 panel wait 降到外层分解循环结束。该路径证明当前框架可以表达跨 panel 依赖，但在 4 vCPU VM 上 `max_dag_live`、依赖边数和 queue 时间显著上升，暂不作为默认。`COMPILER2026_DAG_MAX_LIVE` 已提供一版默认关闭的 live-window drain，可把跨 panel profile 中的 `max_dag_live` 压到窗口附近；当前跨 panel 实验仍未超过默认 panel-local 结果，后续应继续做 per-worker queue/work stealing 或更精确的 critical-path priority，而不是直接默认启用完整跨 panel DAG。

新的受控跨 panel 方向是 `COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1`：不把 `cholesky` 放入 task DAG，而是在原始同步调用前用 `runtime_wait_key` 只等待 diagonal input 的 latest producer。它保留跨 panel `trsm/madd` 依赖表达，但减少 cholesky task 和 context 开销；`runtime_wait_key` 已从超时轮询改为存在 key waiter 时由完成方通知，`COMPILER2026_DAG_MAX_LIVE=2048` repeat=3 有小幅改善但仍未超过默认，因此继续作为 opt-in 实验和后续调度结构的验证入口。

### 3. 运行时任务粒度自适应

单个 `madd` task 对小 `b` 可能过细，对大 `b` 又足够重。运行时应支持：

- 小 task 合并为 range task。
- 大 task 保持单算子粒度以提升负载均衡。
- 根据 `n, b, block_count, thread_count` 自动选择粒度。
- 收集轻量 profile，为下一次调用选择阈值。

当前已具备第一版 profiling 开关：`COMPILER2026_DAG_PROFILE=1` 会输出 async path 判定次数和原因、task 数、队列等待、执行时间、worker idle、`wait()` 调用次数和总耗时、`wait()` 入口 ready/active/DAG live pressure、主线程 wait 空等、批量出队、ready queue 宽度采样、DAG 节点/边/已满足依赖/缺失依赖/first-touch 输入依赖/释放批量/fanout/live 信息，以及按 Pass 注册名称聚合的 `trsm/madd` 统计。benchmark 脚本已经能把这些统计沉淀为 CSV 字段，并派生 `ready_avg` / `ready_per_thread` 观察 ready 宽度是否足以覆盖线程数，派生 `wait_ready_avg` / `wait_active_avg` / `wait_dag_live_avg` 观察 panel barrier 入口仍有多少可执行或未完成工作。Pass 入口已经改为 runtime predicate，`COMPILER2026_ASYNC_MIN_B`、`COMPILER2026_ASYNC_MIN_BLOCKS` 和 `COMPILER2026_DAG_THREADS` 能真实控制 async path；默认 task batch 也已开始参考 `b`、block 数和线程数，避免小 panel 过度批量出队。后续可以继续把这些 profile 数据用于驱动默认异步阈值、未来 range task 和跨 panel DAG 的收益判断。

短期调参策略是“事前离线搜索，运行时廉价决策”：`tune_params.sh` 负责在目标机器上系统扫阈值、最小 block 数、batch、live-window 和线程数；contestant 运行时只根据 `n, b, block_count, thread_count` 做 deterministic heuristic，不在被计时执行中做 warmup 或多参数计时搜索。

### 4. 多核和 NUMA 亲和性

真实鲲鹏平台上需要测试：

- worker pinning。
- work stealing 和 per-worker deque。
- NUMA-aware task placement。
- 避免单全局队列在 48/64 核上成为瓶颈。

当前单锁队列适合验证 IR pass 方向，但不是大核数最终形态。DAG successor 边已经从每个 node 一个小 vector 改为统一 edge pool，减少了 fanout 边的小分配；这降低了当前全局队列模型的构图开销，但不替代 per-worker deque/work stealing。

当前提交已经先落地了一层轻量缓解：runtime 对小/中等 `b` 使用默认上限为 `8` 的小批量提交和批量出队，降低公开 VM 上 `madd` 密集阶段的锁竞争。`COMPILER2026_DAG_PIN_WORKERS=1` 提供默认关闭的 Linux worker 亲和性实验入口，可在真实目标机上和线程数、NUMA 绑定一起测试；后续在 32 核以上平台仍应继续评估 per-worker deque、work stealing 和 NUMA-aware placement。

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
- 跨 panel DAG 已有 opt-in 实验路径，profile 已能把 first-touch 输入依赖和 missing producer 总数同时记录；直接删除 completed latest producer 会把大量 satisfied producer 改写成 missing producer 且性能退化，后续重点应转向 live DAG 内存/锁开销、关键路径优先级和队列扩展性，不能只靠移除 panel wait 或擦除 producer 表直接默认启用。
- live-window drain 和 sync-cholesky key wait 能缓解跨 panel DAG 的 live/task 化压力，但没有解决单全局队列、依赖维护和关键路径优先级问题。
- 阈值仍来自经验测试；async 入口已由 runtime predicate 按 block size、最小 block count 和 thread count 控制，默认 task batch 已开始参考 block count/thread count，runtime 和 benchmark 已能记录 profile，`tune_params.sh` 已能生成离线 aggregate CSV，但尚未形成跨运行的自动 profile-guided heuristic。

短期目标是把 `trsm/madd` 的坐标和依赖边从 IR 中恢复出来；中期目标是生成 ready-queue DAG；长期目标是把这个 pass 做成可解释、可迁移的 tiled linear algebra taskization pass。
