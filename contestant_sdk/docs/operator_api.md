# 算子接口说明书

## 导出 ABI

公开静态库 `libkernels.a` 与评测系统内部使用的 judge 静态库都导出以下 C ABI：

```c
void cholesky(double *A, double *L, int b, int lda);
void trsm(double *A, double *L, double *X, int b, int lda);
void madd(double *A, double *B, double *C, int b, int lda);
```

## 语义

- `cholesky`：对当前对角块做 Cholesky 分解
- `trsm`：基于当前对角块更新同列下方块
- `madd`：执行 `C = C - A * B^T`

## 公开范围

参赛者可获得：

- `libkernels.a`
- 对应的公开源码 `src/base_kernels/`

公开源码的目的是帮助选手理解算子行为并进行本地调试，不代表评测时允许替换官方实现。

## 数据约定

- 所有矩阵都按行主序存储
- `lda` 是全局矩阵的 leading dimension
- 测试用例空间应同时覆盖矩阵规模 `n` 与分块大小 `b`
- 当前仓库中的 baseline 和本地 generator 已支持不同 `b` 的测例，但发布版本地 scaffold 仍要求 `n % b == 0`
- 若后续要支持尾块尺寸不等的正式赛题版本，建议在基础算子 ABI 上显式扩展块尺寸信息，而不是继续复用当前单一 `b` 参数

## 反作弊约束

- 参赛者可以阅读公开源码，但提交物不得重定义、替换或绕过同名核心算子
- 评测阶段会按场景强制链接官方 judge 库
