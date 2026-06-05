# Local Verifier

本地验证工具源码位于 `tools/local_verifier/verifier.cpp`。
若发布包中已包含 `bin/verifier`，可直接使用该预编二进制。

输入格式中的每个测例都同时包含：

- 矩阵规模 `n`
- 分块大小 `b`

因此本地验证时也建议覆盖不同 `(n, b)` 组合，而不是只覆盖固定 `b` 下的不同矩阵规模。

推荐用法：

```bash
./bin/verifier input.bin output.bin
```

若需要加速大矩阵校验，可通过 `VERIFIER_THREADS` 指定并行线程数：

```bash
VERIFIER_THREADS=8 ./bin/verifier input.bin output.bin
```

当前仓库在未显式设置时，会优先选择不超过 `96` 的默认线程数；
在当前 `128` 核 Kunpeng-920 环境中，这通常比直接使用 `128` 线程更高效。

若没有预编二进制，可自行编译：

```bash
cmake -S . -B build
cmake --build build -j
./bin/verifier input.bin output.bin
```
