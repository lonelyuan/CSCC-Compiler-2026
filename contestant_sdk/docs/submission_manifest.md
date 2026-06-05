# 提交物 Manifest 说明

评测系统不会直接运行参赛者自己链接出的应用程序，而是会读取 `manifest.json`，基于官方 baseline 语义基线重新生成 judge-side 待测二进制。

这些 judge-side 二进制的生成和运行细节由评测系统控制，不属于参赛者接口的一部分。

## 最小格式

```json
{
  "pass_plugin": "pass/libcontestant_pass.so",
  "pass_pipeline": "contestant-pass",
  "runtime_libs": [
    "runtime/libcontestant_runtime.a"
  ],
  "runtime_objects": [],
  "compile_flags": [],
  "link_flags": [
    "-pthread"
  ],
  "pass_opt_flags": []
}
```

## 字段说明

- `pass_plugin`
  必填。参赛者 Pass 插件产物路径

- `pass_pipeline`
  必填。传给 `opt -passes=` 的 pipeline 名称

- `runtime_libs`
  可选。选手运行时静态库列表

- `runtime_objects`
  可选。除静态库外，还需要直接参与链接的对象文件列表

- `compile_flags`
  可选。baseline 编译输入在编译为 LLVM bitcode 时需要追加的编译参数

- `link_flags`
  可选。额外链接参数，例如 `-pthread`

- `pass_opt_flags`
  可选。传给 `opt` 的额外参数

## 路径解析规则

若路径不是绝对路径，评测脚本按以下顺序查找：

1. `submission_dir/<path>`
2. `build_dir/<path>`
3. `manifest.json` 所在目录下的相对路径

通常最稳妥的写法是让路径直接相对于 `build_dir`。

仓库自带的 `template/` 已按这个约定输出到：

- `build_dir/pass/libcontestant_pass.so`
- `build_dir/runtime/libcontestant_runtime.a`

因此它可以直接作为一个串行基线提交物，用来验证官方 build flow。

## 可选的标注源码副本

如果参赛者需要通过源码标注辅助 Pass 分析，可以额外在提交目录中提供：

- `src/baseline/main.cpp`
- `src/baseline/block_cholesky.cpp`

这些文件不会直接替换官方 baseline。评测系统会先执行合法性检查：

- 去掉允许的标注语法后，必须与官方 baseline 完全一致
- 若存在除标注外的源码差异，构建会直接失败

当前允许的标注语法见 [annotation_spec.md](annotation_spec.md)。

## 重要约束

- 正式评测时，官方脚本会使用只读 baseline 源码重新生成最终应用
- 正式评测时，脚本会使用 `-Wl,--whole-archive` 强制拉入官方 judge 静态库
- 参赛者提交物不得重定义、替换或绕过官方 `cholesky`、`trsm`、`madd`
- judge-side 校验与计时变体的具体划分属于评测系统内部实现
