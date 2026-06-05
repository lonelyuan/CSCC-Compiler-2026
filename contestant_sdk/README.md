# Contestant SDK

这个目录是准备发给参赛者的公开包。

包含内容：

- `bin/verifier`：可选的本地结果校验器预编二进制
- `bin/spd_generator`：可选的本地 SPD 生成器预编二进制
- `include/kernels.h`：官方算子 ABI
- `cases/preliminary_public_150.txt`：公开 150 组初赛用例规格
- `lib/libkernels.a`：公开静态库
- `src/base_kernels/`：公开基础算子源码
- `src/baseline/`：baseline 源码
- `docs/`：公开文档
- `template/`：可直接构建的 LLVM Pass 与 runtime 基线模板
- `template/manifest.json`：提交物 manifest 示例
- `tools/public_cases/`：公开初赛用例规格生成脚本
- `tools/local_generator/`：本地 SPD 生成器源码
- `tools/local_verifier/`：本地校验器源码与说明

额外说明：

- 提供基础算子源码是为了帮助参赛者理解接口和本地调试
- `cases/preliminary_public_150.txt` 是面向参赛者公开的初赛本地验证集规格文件
- `template/` 本身可以作为“串行基线提交物”直接跑通官方 build flow
- `template/src/baseline/` 可作为“仅加标注”的 baseline 源码副本编辑入口
- 正式评测时仍由赛方强制链接官方版本算子库
