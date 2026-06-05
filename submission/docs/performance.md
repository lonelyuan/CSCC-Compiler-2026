# 性能记录

本文件记录正式 `submission/runtime` 的优化实验。所有数据来自 openEuler aarch64 VM，使用毕昇 3.2.0.1 / LLVM 15.0.4，`COMPILER2026_DAG_THREADS=4`，每组重复 3 次。

原始 CSV 位置：

```text
build/optimization_benchmarks/before_runtime_opt.csv
build/optimization_benchmarks/after_runtime_opt.csv
build/optimization_benchmarks/after_madd_coarsening.csv
```

已归档到 git 的 CSV 位置：

```text
submission/docs/benchmark_results/before_runtime_opt.csv
submission/docs/benchmark_results/after_runtime_opt.csv
submission/docs/benchmark_results/after_madd_coarsening.csv
```

VM 中对应位置：

```text
/root/bisheng/build/optimization_benchmarks/before_runtime_opt.csv
/root/bisheng/build/optimization_benchmarks/after_runtime_opt.csv
/root/bisheng/build/optimization_benchmarks/after_madd_coarsening.csv
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
LABEL=after_madd_coarsening REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

## 优化阶段

### before_runtime_opt

第一版正式 DAG runtime：

- Pass 将 `contest::block_cholesky` 替换为 runtime 入口。
- Runtime 使用 panel barrier 调度。
- `trsm` 并行。
- `madd` 以单 block 更新为任务粒度。

### after_runtime_opt

低风险 runtime 开销优化：

- 矩阵 `A -> L` 拷贝按行并行。
- `madd` 任务列表跨 panel 复用，减少重复分配。
- 线程池按 chunk 抢任务，减少每个任务一次 atomic 的开销。
- runtime 线程数不超过 block 数，避免明显过量线程。

### after_madd_coarsening

小 block 任务粒度优化：

- 当 `b <= 32` 时，把同一 `col_block` 下的多个 `madd` 合并为一个任务。
- 目的：减少小 block 场景下大量细粒度 `madd` 的调度成本。
- 当 `b > 32` 时仍使用单 block `madd` 任务，保留大 block 的并行度。

## 结果摘要

| Suite | before speedup | after runtime opt | after madd coarsening |
| --- | ---: | ---: | ---: |
| `n512_576` | 1.021x | 1.114x | 1.181x |
| `n768` | 1.649x | 1.751x | 1.857x |
| `n1024` | 1.883x | 2.032x | 2.171x |
| `n1152_small_b` | 1.567x | 1.850x | 2.065x |

Contestant 平均时间：

| Suite | before | after runtime opt | after madd coarsening |
| --- | ---: | ---: | ---: |
| `n512_576` | 0.078410s | 0.072149s | 0.068599s |
| `n768` | 0.126522s | 0.119467s | 0.114031s |
| `n1024` | 0.148902s | 0.139720s | 0.130749s |
| `n1152_small_b` | 0.226227s | 0.181698s | 0.164744s |

## 结论

本轮优化后，四组公开区间全部提升，且 benchmark 脚本中的 contestant 输出均通过 verifier。

收益最大的场景是小 block 密集任务：

- `n1152_small_b` 从 1.567x 提升到 2.065x。
- `n1024` 从 1.883x 提升到 2.171x。

后续优化方向：

- 将 panel barrier 进一步推进到异步 DAG，允许下一 panel 在必要 block 更新完成后尽早启动。
- 针对 `b` 和 `block_count` 自动选择 `madd` 合并阈值。
- 减少每个 panel 的同步次数，或者把 `trsm` 和 `madd` 任务放入统一 ready queue。
