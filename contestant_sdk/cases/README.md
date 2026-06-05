# Public Preliminary Cases

本目录存放面向参赛者公开的初赛用例规格文件。

当前提供：

- `preliminary_public_150.txt`
  公开 150 组 `(n, b, seed)` 规格，每个 `(n, b)` 组合保留一个代表性 seed。

使用原则：

- 这份公开文件主要用于本地联调、性能对比和提交前自测。
- 公开文件中的矩阵内容可由 `spd_generator --spec` 在本地确定性生成。
- 官方线上评测应以相同的 `(n, b)` 分布为主，但可以重新生成矩阵数据，不要求与这份公开文件中的具体数值完全一致。

推荐生成方式：

```bash
./bin/spd_generator public_preliminary.bin --spec cases/preliminary_public_150.txt
```

若生成较大规格较慢，可通过环境变量控制并行线程数，例如：

```bash
SPD_GENERATOR_THREADS=8 ./bin/spd_generator public_preliminary.bin --spec cases/preliminary_public_150.txt
```

当前仓库在未显式设置时，会优先选择不超过 `96` 的默认线程数；
若在 `128` 核 Kunpeng-920 上运行，这比直接拉满 `128` 线程通常更高效。
