# Runnable Serial Baseline Submission

这个目录现在不仅是模板，也是一个可直接提交到官方 judge flow 中的最小可运行基线。

它的行为是：

- 提供一个合法的 no-op LLVM Pass 插件
- 提供一个最小运行时静态库
- 提供一份可选的 `src/baseline/` baseline 源码副本，供参赛者仅添加标注
- 除合法标注外，不改变 baseline 的 IR 与执行顺序
- 因此最终生成的 judge-side 可执行文件在功能上等价于官方串行 baseline

在 `contestant_sdk` 包内，可先直接构建模板产物：

```bash
cmake -S template -B build/template_baseline
cmake --build build/template_baseline -j
```

若要跑官方完整评测链路，请在 `judge_runtime` 发布包或源码仓库根目录中执行
`build_submission.sh` / `judge_submission.sh`；这些脚本不包含在
`contestant_sdk` 包内。发布包形态下可参考：

```bash
cd <judge_runtime_root>
./scripts/build_submission.sh <contestant_sdk_root>/template <build_dir>
./scripts/judge_submission.sh <contestant_sdk_root>/template <workspace_dir> baseline --spec-file cases.txt
```

说明：

- 需要本机具备 LLVM 开发包，并能找到 `opt`、`llvm-link`、`clang++`
- `manifest.json` 中的相对路径按官方脚本约定解析到 `build/` 里的 `pass/` 和 `runtime/`
- 若修改 `src/baseline/*.cpp`，只允许加入 [annotation_spec.md](../docs/annotation_spec.md) 中定义的合法标注；judge 会在构建前校验“去标注后源码完全一致”
