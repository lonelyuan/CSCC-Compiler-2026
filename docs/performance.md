# 性能记录

本文件记录当前正式 IR-level Pass 的性能。所有数据来自 openEuler aarch64 VM，使用毕昇 3.2.0.1 / LLVM 15.0.4，`COMPILER2026_DAG_THREADS=4`，每组重复 3 次。

## 当前有效结果

当前交付版本是 `ir_async_threshold32`：

```text
docs/benchmark_results/ir_async_threshold32.csv
```

VM 原始输出：

```text
/root/bisheng/build/optimization_benchmarks/pass_runtime_threshold32.csv
```

结果摘要：

| Suite | serial avg | contestant avg | speedup |
| --- | ---: | ---: | ---: |
| `n512_576` | 0.092441s | 0.075444s | 1.227x |
| `n768` | 0.228311s | 0.130538s | 1.749x |
| `n1024` | 0.306771s | 0.167911s | 1.827x |
| `n1152_small_b` | 0.361752s | 0.306669s | 1.180x |

四个 suite 平均加速比的几何平均约为 `1.467x`。所有 contestant 输出均通过 verifier。

## 本轮优化变化

相对上一版 `ir_outlined_task_pass`，本轮保留 IR-level 算子任务化路线，但做了以下 runtime/阈值优化：

- async 阈值从 `b >= 64` 调整为 `b >= 32`，并确保 Pass 入口分支和 runtime 默认阈值一致。
- runtime 由每次 `block_cholesky` 调用创建/销毁 worker 改为 thread-local worker 池复用，worker 数变化时才重建。
- task context 改为 arena 分配，避免每个 `trsm/madd` task 单独 `malloc/free`。
- `wait()` 中主线程参与执行队列任务，使配置的线程数近似为 `main + workers`。
- 任务队列从 `deque` 改为可复用 vector 队列，并按当前 block 数预留容量。
- 提交端减少重复 `notify_one`，降低大量小 task 入队时的条件变量通知开销。

`b >= 16` 也做过实验，但在公开 benchmark 中触发段错误，已回退，不作为可交付配置。

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
LABEL=pass_runtime_threshold32 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

## 历史结果说明

仓库中还保留了以下 CSV：

```text
docs/benchmark_results/before_runtime_opt.csv
docs/benchmark_results/after_runtime_opt.csv
docs/benchmark_results/after_madd_coarsening.csv
docs/benchmark_results/ir_loop_pass_final.csv
docs/benchmark_results/ir_outlined_task_pass.csv
```

前三个 CSV 来自早期“整函数替换为 runtime 入口”的实验版本。它们的性能更高，但该路线不够符合赛题对 IR 层算子依赖分析的要求，因此不作为当前提交方案。

## 结论

当前方案仍是保守的 panel-barrier DAG：

- Pass 分析官方 baseline IR 中的 `trsm/madd` call 和 loop exit。
- 原始 `block_cholesky` 保留为小 block 串行路径。
- async clone 中的 `trsm/madd` call site 被替换为通用任务提交。
- Pass 生成的 task function 内直接调用官方 `trsm/madd` ABI。
- runtime 不包含算子专用 wrapper，不替换官方算子实现。

后续更大的性能空间来自真正的 block-coordinate ready queue DAG：让下一 panel 在其依赖 block 更新完成后提前启动，而不是等待整个 trailing matrix 更新完成。
