# 设计说明

## 目标

本提交物通过 LLVM Pass 将官方 baseline 中的 `contest::block_cholesky` 重定向到参赛运行时库，由运行时库构造并执行分块 Cholesky 的 tile-level DAG。运行时库只调度官方 `cholesky`、`trsm`、`madd` 算子，不替换、不重定义、不绕过官方算子。

## Pass 设计

Pass 名称：

```text
contestant-pass
```

Pass 插件：

```text
pass/libcontestant_pass.so
```

Pass 查找满足以下条件的函数：

- 函数名为官方 C++ mangled name `_ZN7contest14block_choleskyEPKdPdii`，或包含 `block_cholesky`。
- 返回类型为 `i32`。
- 参数数量为 4。

匹配后，Pass 清空原函数体并生成：

```text
return compiler2026_block_cholesky_runtime(A, L, n, b);
```

运行时入口使用 C ABI：

```c
extern "C" int compiler2026_block_cholesky_runtime(
    const double *A,
    double *L,
    int n,
    int b
);
```

## Runtime 设计

Runtime 静态库：

```text
runtime/libcontestant_runtime.a
```

Runtime 负责：

1. 校验输入参数。
2. 将输入矩阵 `A` 拷贝到输出矩阵 `L`。
3. 按 block 坐标执行分块 Cholesky。
4. 使用线程池并行执行同一 panel 内可并行的 `trsm` 和 `madd`。
5. 清零上三角。

当前调度为 barrier DAG：

```text
for panel:
  cholesky(panel, panel)
  parallel trsm(row, panel)
  parallel madd(row, col, panel)
clear upper triangle
```

依赖关系：

- `trsm(row, panel)` 依赖 `cholesky(panel, panel)`。
- `madd(row, col, panel)` 依赖 `trsm(row, panel)` 和 `trsm(col, panel)`。
- 下一轮 `cholesky(panel + 1, panel + 1)` 依赖上一轮所有更新该对角块的 `madd`。

该调度比完全异步 DAG 保守，但语义清晰，便于正确性验证，是后续细粒度 DAG 优化的基础版本。

## 并行参数

线程数通过环境变量配置：

```bash
COMPILER2026_DAG_THREADS=4
```

若未设置，则使用 `std::thread::hardware_concurrency()`。

该环境变量用于本地调试和调参。正式提交不依赖源码标注传递平台特定参数。

## 正确性保证

Runtime 使用与官方 baseline 相同的数据布局和算子调用：

```c
void cholesky(double *A, double *L, int b, int lda);
void trsm(double *A, double *L, double *X, int b, int lda);
void madd(double *A, double *B, double *C, int b, int lda);
```

每个 `madd` 任务写入唯一的 `(row, col)` block。同一 panel 的 `madd` 任务之间没有写写冲突。panel 间通过 barrier 保证依赖顺序。

## 当前限制

- 当前 runtime 要求 `n % b == 0`，与 SDK 当前公开 scaffold 一致。
- 当前版本为 panel barrier 调度，不是完全异步 DAG。
- 当前版本未做绑核、NUMA、任务合并等性能优化。

## 本地验证

在 openEuler/BiSheng VM 中执行：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh
```

脚本会：

1. 构建 Pass 和 runtime。
2. 将官方 baseline 编译为 LLVM bitcode。
3. 使用 `opt -passes=contestant-pass` 应用 Pass。
4. 链接 runtime 和公开基础算子源码。
5. 生成测试矩阵。
6. 运行串行 baseline 和 contestant app。
7. 使用 verifier 校验两份输出。
8. 输出性能对比。

当前已验证结果：

```text
SPEC_START=43 SPEC_END=56 COMPILER2026_DAG_THREADS=4
serial_seconds=0.080102832
contestant_seconds=0.070966918
speedup=1.129x

SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4
serial_seconds=0.275685036
contestant_seconds=0.149214601
speedup=1.848x
```

上述两组测试的串行 baseline 输出和 contestant app 输出均通过 verifier。

更新后的详细性能记录见 `docs/performance.md`。当前优化版本在 `n1024` 公开子集上达到约 `2.171x` 平均加速，在 `n1152_small_b` 子集上达到约 `2.065x` 平均加速。
