# Bisheng Compiler Contest Project

本仓库是“动态算子图编译与并行调度”赛题的开发工程。项目目标是在 LLVM/BiSheng 编译器中增加 Pass，从官方分块 Cholesky baseline 的 IR 中识别 `cholesky`、`trsm`、`madd` 算子调用，分析可并行的算子依赖关系，并通过运行时任务调度提升多核执行性能。

当前实现遵循编译器优化路线：不重写官方算法源码，不替换官方算子实现；Pass 在 IR 层克隆 async 版本、outline 算子任务函数、恢复 block key，并插入 runtime submit/wait 或 submit_deps 调用。官方 `trsm` 和 `madd` ABI 调用保留在 Pass 生成的 IR task function 内。

## 项目结构

```text
.
├── README.md
├── DEVELOPMENT_GUIDE.md
├── docs/
│   ├── design.md
│   ├── optimization_principles.md
│   ├── performance.md
│   ├── roadmap.md
│   ├── engineering_log.md
│   ├── judge_automation.md
│   └── benchmark_results/
├── submission/
│   ├── CMakeLists.txt
│   ├── README.md
│   ├── manifest.json
│   ├── pass/
│   │   ├── CMakeLists.txt
│   │   └── dag_pass.cpp
│   ├── runtime/
│   │   ├── CMakeLists.txt
│   │   └── dag_runtime.cpp
│   └── scripts/
│       ├── build.sh
│       ├── smoke_test.sh
│       ├── benchmark.sh
│       └── package.sh
├── scripts/
│   └── sync_to_vm.sh
├── skills/
│   └── compiler-contest-assistant/
├── contestant_sdk/
├── build/
└── dist/
```

## 关键目录

- `submission/`：竞赛提交工程，评测平台会从这里构建 LLVM Pass 和 runtime。
- `submission/pass/dag_pass.cpp`：LLVM Pass 实现，负责 IR 版本化、算子调用识别、task function 生成和同步点插入。
- `submission/runtime/dag_runtime.cpp`：通用任务运行时，负责 worker 池、任务队列、arena 分配、wait 协作执行和可选 profiling。
- `docs/`：项目文档，已经从 `submission/docs` 提升到仓库根目录，便于统一维护。
- `docs/optimization_principles.md`：面向基础编程读者的并行优化和算子图调度原理说明。
- `docs/design.md`：实现级设计说明。
- `docs/performance.md`：性能实验结果和 CSV 路径。
- `docs/roadmap.md`：面向真实鲲鹏多核平台和决赛扩展数据的长期优化路线。
- `docs/engineering_log.md`：记录每轮优化的验证结果、经验教训和后续约束。
- `docs/judge_automation.md`：提交前自动化、平台上传确认边界与云桌面协作约定。
- `docs/benchmark_results/`：历史和当前 benchmark 结果。
- 赛题 PDF 可作为本地参考文件放在 `docs/` 下；PDF 文件默认被 `.gitignore` 忽略，不作为工程源码提交。
- `contestant_sdk/`：官方 SDK、baseline、测试工具和公开 case。
- `scripts/sync_to_vm.sh`：把本地工程同步到 openEuler/BiSheng 虚拟机。
- `skills/compiler-contest-assistant/`：本项目配套 Codex skill，记录开发环境、比赛约束、评测流程和长期优化路线，便于协作者复用。
- `dist/`：本地打包输出目录，包含 `submission.zip` 等提交包。
- `tools/sched_harness/`：本机调度可行性 harness（仅调试用，不在 `submission/` 内，不影响评测包）。它链接真实 runtime 并复刻分块 Cholesky 任务 DAG，用于在本机多核（如 Apple M5）上验证跨 panel 调度与 work-stealing 的扩展性机制，弥补 4 核 VM 无法暴露大核数收益的限制。

## Codex Skill

本仓库包含一个项目专用 skill：

```text
skills/compiler-contest-assistant/
```

协作者可以复制或软链接到自己的 Codex skills 目录：

```bash
mkdir -p ~/.codex/skills
ln -s "$(pwd)/skills/compiler-contest-assistant" ~/.codex/skills/compiler-contest-assistant
```

之后在 Codex 中显式调用：

```text
$compiler-contest-assistant 继续优化本项目
```

该 skill 会提醒 Codex 使用 openEuler/BiSheng VM、遵守比赛规则、不修改 SDK/算子实现、不做虚假优化，并优先从 IR 依赖分析、任务图调度和 runtime 扩展性角度推进项目。

## 常用命令

同步到虚拟机：

```bash
./scripts/sync_to_vm.sh
```

在虚拟机中构建提交工程：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
./submission/scripts/build.sh
```

运行 smoke test：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh
```

运行 benchmark：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
LABEL=runtime_ready_queue_trsm_deps REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

打开 runtime profile：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
SPEC_START=93 SPEC_END=93 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/smoke_test.sh
```

benchmark 中打开同一开关会把 profile 字段写入 CSV：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
COMPILER2026_DAG_PROFILE=1 LABEL=ready_queue_profile_csv_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

生成提交包：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
./submission/scripts/package.sh
```

提交包会生成：

```text
dist/submission.zip
dist/submission.tar.gz
```

其中 `submission.zip` 的压缩包根目录直接包含 `CMakeLists.txt`，适配评测平台直接对解压目录运行 CMake 的流程。
