# 性能记录

本文件记录当前正式 IR-level Pass 的性能。所有数据来自 openEuler aarch64 VM，使用毕昇 3.2.0.1 / LLVM 15.0.4，`COMPILER2026_DAG_THREADS=4`，每组重复 3 次。

## 当前有效结果

当前交付版本是 `ir_loop_pass_final`：

```text
submission/docs/benchmark_results/ir_loop_pass_final.csv
```

VM 原始输出：

```text
/root/bisheng/build/optimization_benchmarks/ir_loop_pass_final.csv
```

结果摘要：

| Suite | serial avg | contestant avg | speedup |
| --- | ---: | ---: | ---: |
| `n512_576` | 0.088756s | 0.085000s | 1.044x |
| `n768` | 0.232610s | 0.158435s | 1.468x |
| `n1024` | 0.308554s | 0.201376s | 1.532x |
| `n1152_small_b` | 0.363727s | 0.383082s | 0.950x |

所有 contestant 输出均通过 verifier。

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
LABEL=ir_loop_pass_final REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

## 历史结果说明

仓库中还保留了以下 CSV：

```text
submission/docs/benchmark_results/before_runtime_opt.csv
submission/docs/benchmark_results/after_runtime_opt.csv
submission/docs/benchmark_results/after_madd_coarsening.csv
```

这些数据来自早期“整函数替换为 runtime 入口”的实验版本。它们的性能更高，但该路线不够符合编译器赛题对 IR 层算子依赖分析的要求，因此不再作为当前提交方案。

## 结论

当前 IR-level Pass 相比旧函数替换版本性能下降，但结构更符合赛题要求：

- Pass 直接分析官方 baseline IR。
- Pass 基于 `LoopInfo` 插入 runtime begin/submit/wait/end。
- 原始 `block_cholesky` 循环结构仍保留。
- `trsm/madd` 原始 call 在 optimized path 中被替换为异步任务提交。
- 小 block 走由原始 IR 克隆出的 serial fallback。

后续优化方向：

- 不再只做 panel barrier，而是构建真正的异步 DAG ready queue。
- 在 Pass 层识别 block 坐标表达式，为 `madd` 合并和依赖注册提供 IR 级信息。
- 对小 block fallback 进一步降低版本化开销。
- 将 `b >= 64` 的阈值改为 runtime/profile 驱动策略。

