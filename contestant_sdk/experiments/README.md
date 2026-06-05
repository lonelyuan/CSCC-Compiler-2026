# Baseline 与最小优化实验

本目录用于记录当前阶段的性能基线和最小优化验证。这里的优化实验用于验证开发、计时和校验链路是否能观测到加速，不作为最终合规提交方案。

## 实验内容

新增程序：

```text
experiments/case_parallel_main.cpp
```

它复用官方 baseline 的 `contest::block_cholesky`，不改 `cholesky`、`trsm`、`madd` 算子实现，只把输入文件中的多个独立 case 分配到多个线程上执行。

对比对象：

- `baseline_serial`：原始串行处理所有 case。
- `case_parallel`：多个 case 并行处理，每个 case 内部仍使用原始串行 `block_cholesky`。

这个优化的意义：

- 快速建立可重复的性能 baseline。
- 验证 `COMPILER2026_TIMING_FILE` 计时链路可用。
- 验证并行执行后的输出仍能通过 verifier。
- 观察线程数与加速效果的关系。

限制：

- 该实验直接新增了一个替代 main 程序，不是最终比赛提交形态。
- 最终提交仍应通过 LLVM Pass 和 runtime 对官方 baseline 做合法转换。
- case 级并行只利用不同输入 case 之间的独立性，后续仍需要实现单个矩阵内部的 tile DAG 并行。

## 运行方式

在 openEuler VM 中执行：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng/contestant_sdk

CASE_THREADS=4 \
GENERATOR_THREADS=4 \
VERIFIER_THREADS=4 \
  ./experiments/run_case_parallel_benchmark.sh
```

默认使用公开规格文件的第 43 到 56 行：

```text
512:8:4201
512:16:4301
512:32:4401
512:64:4501
512:128:4601
512:256:4701
576:8:4801
576:9:4901
576:12:5001
576:16:5101
576:18:5201
576:24:5301
576:32:5401
576:36:5501
```

可以通过环境变量调整测试范围：

```bash
SPEC_START=71 SPEC_END=81 CASE_THREADS=4 ./experiments/run_case_parallel_benchmark.sh
```

## 当前验证结果

环境：

```text
openEuler 22.03 LTS aarch64 VM
BiSheng Enterprise 3.2.0.1.B004 clang version 15.0.4
nproc = 4
```

单轮脚本结果：

```text
serial_seconds=0.079242750
case_parallel_seconds=0.024468206
speedup=3.239x
```

两份输出均已通过 verifier。

5 轮重复结果：

```text
serial avg       0.079403885s
parallel_t1 avg 0.087243011s speedup 0.91x
parallel_t2 avg 0.045870345s speedup 1.73x
parallel_t4 avg 0.025136079s speedup 3.16x
parallel_t8 avg 0.029008995s speedup 2.74x
```

结论：

- case 级并行在当前 4 核 VM 上明确生效。
- 4 线程最优，8 线程过量调度导致收益下降。
- 1 线程版本比串行 baseline 稍慢，说明线程调度框架本身有开销。
- 后续优化应避免过细粒度任务和过量线程。

## 后续迁移方向

下一步应把实验验证过的并行框架思想迁移到比赛要求的形态：

1. 在 runtime 中提供 `compiler2026_block_cholesky_runtime(A, L, n, b)`。
2. 在 LLVM Pass 中识别并替换 `contest::block_cholesky` 的函数体或调用点。
3. runtime 内部构造单个矩阵的 tile-level DAG。
4. 任务节点调用官方 `cholesky`、`trsm`、`madd`。
5. 使用 verifier 检查正确性，再用同一基准脚本扩展性能对比。

