# 动态算子图编译与并行调度开发指南

本文档面向本项目当前环境，目标是帮助快速进入开发状态：在 openEuler aarch64 虚拟机中使用毕昇编译器构建 LLVM Pass、运行时库和本地验证程序，并逐步实现分块 Cholesky 算子图并行调度。

## 1. 项目目标

赛题要求在不修改官方 baseline 算法语义、不替换官方基础算子的前提下，通过 LLVM Pass 分析 `cholesky`、`trsm`、`madd` 的依赖关系，生成并行执行程序，提升分块 Cholesky 分解性能。

核心限制：

- 不允许修改算法源码，合法标注除外。
- 不允许重定义、替换或绕过 `cholesky`、`trsm`、`madd`。
- 提交物由 `manifest.json` 描述，评测系统会重新构建 judge-side 二进制。
- 主要优化空间是算子调用级别的 DAG 构建、依赖管理和并行调度。

核心代码位置：

- `contestant_sdk/src/baseline/block_cholesky.cpp`
- `contestant_sdk/template/llvm-pass/pass_stub.cpp`
- `contestant_sdk/template/runtime/runtime_stub.cpp`
- `contestant_sdk/template/manifest.json`

## 2. 当前开发环境

本机是 macOS ARM，主要用于编辑代码。目标开发和验证环境在 openEuler 机器内。

### 2.1 性能测量首选平台：aarch64 云桌面（Round 15 起）

这是目前**最接近官方性能平台**的机器：同 ISA、同 OS、同编译器版本。任何要写进
`docs/performance.md` 的数字都应该在这里测。

```text
HiSilicon aarch64（SVE / i8mm / bf16），openEuler 22.03 LTS，kernel 5.10
40 核 / 单 NUMA 节点 / 2.9 GHz 定频（boost disabled）/ L3 32 MiB
BiSheng Enterprise 3.2.0.1.B004 clang 15.0.4，装在 /opt/bisheng
项目目录 /root/bisheng
```

经 tailscale 隧道接入（节点名 `bisheng-cg-aarch64`）。主机地址放在**未跟踪**的
`scripts/cg_host.env`（照 `scripts/cg_host.env.example` 填），因为本仓库是公开镜像。

```bash
./scripts/cg.sh 'nproc'                 # 跑一条命令（ssh 多路复用）
./scripts/sync_to_cg.sh                 # 同步工作树到 /root/bisheng
./scripts/cg_run.sh <job> '<command>'   # detach 跑长作业，掉线不丢
./scripts/cg_run.sh --tail <job>        # 看进度
```

长作业一律用 `cg_run.sh`：链路是 relay 中转的 tailscale，实测会掉线，`setsid nohup`
能让作业活下来。

**两个必须知道的环境约束：**

1. **2 GiB 内存 cgroup 硬上限。** `free` 显示的 77 GB 是宿主视图；真实上限在
   `/sys/fs/cgroup/memory/memory.limit_in_bytes`，只读改不了。per-case harness 按官方
   `matrix_case_io` 约定要同时驻留 150 个用例的输入和输出（2.46 GB），会被 OOM killer
   打掉（exit 137）。跑全量用 `scripts/percase_bench_chunked.sh`，它切成 ≤600 MB 的 5 段
   再合并打分。
2. **这台机器是从零装起来的**，如果换了新容器需要重做：

```bash
dnf install -y rsync numactl perf gcc-c++ libstdc++-devel libstdc++-static
curl -L -o /root/dl/bisheng.tar.gz \
  https://mirrors.huaweicloud.com/kunpeng/archive/compiler/bisheng_compiler/BiShengCompiler-3.2.0.1-aarch64-linux.tar.gz
# sha256 14a71269725d871ae3e0fe4345953dcb0c090f8e37a832a59fd3c13d7d7ca236
tar -xzf /root/dl/bisheng.tar.gz -C /opt && mv /opt/BiShengCompiler-3.2.0.1-aarch64-linux /opt/bisheng
cat > /etc/profile.d/bisheng.sh <<'EOF'
export BISHENG_HOME=/opt/bisheng
export PATH="${BISHENG_HOME}/bin:${PATH}"
export LD_LIBRARY_PATH="${BISHENG_HOME}/lib:${LD_LIBRARY_PATH:-}"
EOF
```

`libstdc++-devel` 不能省：毕昇 clang++ 用系统 libstdc++，缺了它 CMake 在
`Check for working CXX compiler` 就报 `cannot find -lstdc++`。

### 2.2 x86_64 Xeon 调试机（对照，非性能平台）

40 物理核 Xeon Gold 5218R ×2 / Ubuntu 22.04 / LLVM 17，入口 `scripts/xeon.sh`。
Round 8–14 的全部调优常量都在这台机器上标定。**Round 15 已证明其中至少 range chunk
预算是平台专属拟合**（那台机器上串行基准慢 1.8 倍，于是"每 task 多少 flops"整条标定失效），
所以这台机器只适合做结构性实验和同机相对比较，不能用来定常量。

### 2.3 旧 4 vCPU 虚拟机（已不可达）

```bash
ssh -i ~/.ssh/bisheng_vm_ed25519 root@192.168.8.131
```

虚拟机项目目录：

```bash
/root/bisheng
```

本机项目目录：

```bash
/Users/chenzhongyuan/Documents/bisheng
```

毕昇编译器安装目录：

```bash
/opt/bisheng
```

登录虚拟机后先加载环境：

```bash
source /etc/profile.d/bisheng.sh
```

确认工具链：

```bash
which clang++
clang++ --version
which llvm-config
llvm-config --version
which opt
opt --version
```

当前已验证的毕昇版本：

```text
BiSheng Enterprise 3.2.0.1.B004 clang version 15.0.4
llvm-config 15.0.4
opt LLVM version 15.0.4
```

## 3. 同步代码

从本机同步到虚拟机：

```bash
rsync -az --delete \
  -e "ssh -i ~/.ssh/bisheng_vm_ed25519" \
  --exclude ".DS_Store" \
  --exclude "build" \
  /Users/chenzhongyuan/Documents/bisheng/ \
  root@192.168.8.131:/root/bisheng/
```

如果只改了少量文件，也可以同步单个文件，例如：

```bash
scp -i ~/.ssh/bisheng_vm_ed25519 \
  /Users/chenzhongyuan/Documents/bisheng/contestant_sdk/template/llvm-pass/pass_stub.cpp \
  root@192.168.8.131:/root/bisheng/contestant_sdk/template/llvm-pass/pass_stub.cpp
```

## 4. 构建模板提交物

在虚拟机内执行：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng/contestant_sdk

cmake -S template -B build/template_bisheng -G Ninja \
  -DLLVM_CONFIG=/opt/bisheng/bin/llvm-config \
  -DCMAKE_C_COMPILER=/opt/bisheng/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/bisheng/bin/clang++

cmake --build build/template_bisheng -j$(nproc)
```

成功后应生成：

```text
build/template_bisheng/pass/libcontestant_pass.so
build/template_bisheng/runtime/libcontestant_runtime.a
build/template_bisheng/manifest.json
```

验证 Pass 插件能被 `opt` 加载：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng/contestant_sdk

mkdir -p build/local/ir

clang++ -std=c++17 -Iinclude -Isrc/base_kernels \
  -emit-llvm -c src/baseline/block_cholesky.cpp \
  -o build/local/ir/block_cholesky.bisheng.bc

opt -load-pass-plugin build/template_bisheng/pass/libcontestant_pass.so \
  -passes=contestant-pass \
  build/local/ir/block_cholesky.bisheng.bc \
  -o build/local/ir/block_cholesky.bisheng.opt.bc
```

如果命令无报错，说明 Pass 插件 ABI 和毕昇 LLVM 版本匹配。

## 5. 构建本地验证工具

SDK 自带的 `bin/spd_generator`、`bin/verifier` 和 `lib/libkernels.a` 是 x86-64 产物，不能直接在 aarch64 openEuler 虚拟机中使用。当前需要从源码构建 aarch64 版本。

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng/contestant_sdk

mkdir -p build/bisheng_local/bin

clang++ -std=c++17 -O2 -pthread -Iinclude \
  tools/local_generator/spd_generator.cpp \
  -o build/bisheng_local/bin/spd_generator

clang++ -std=c++17 -O2 -pthread -Iinclude \
  tools/local_verifier/verifier.cpp \
  -o build/bisheng_local/bin/verifier

clang++ -std=c++17 -O2 -pthread \
  -Iinclude -Isrc/base_kernels \
  src/base_kernels/kernels_public.cpp \
  src/base_kernels/kernels_impl.cpp \
  src/baseline/main.cpp \
  src/baseline/block_cholesky.cpp \
  -o build/bisheng_local/bin/baseline_serial
```

## 6. 跑通最小样例

单 case 验证：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng/contestant_sdk

./build/bisheng_local/bin/spd_generator \
  build/bisheng_local/smoke_128_16.bin \
  128 16 1

COMPILER2026_TIMING_FILE=build/bisheng_local/smoke_128_16.time \
  ./build/bisheng_local/bin/baseline_serial \
  build/bisheng_local/smoke_128_16.bin \
  build/bisheng_local/smoke_128_16.out

./build/bisheng_local/bin/verifier \
  build/bisheng_local/smoke_128_16.bin \
  build/bisheng_local/smoke_128_16.out

cat build/bisheng_local/smoke_128_16.time
```

预期结果包含：

```text
status=PASS
```

跑公开用例前 5 个：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng/contestant_sdk

head -5 cases/preliminary_public_150.txt > build/bisheng_local/smoke5.spec

SPD_GENERATOR_THREADS=4 \
  ./build/bisheng_local/bin/spd_generator \
  build/bisheng_local/smoke5.bin \
  --spec build/bisheng_local/smoke5.spec

COMPILER2026_TIMING_FILE=build/bisheng_local/smoke5.time \
  ./build/bisheng_local/bin/baseline_serial \
  build/bisheng_local/smoke5.bin \
  build/bisheng_local/smoke5.out

VERIFIER_THREADS=4 \
  ./build/bisheng_local/bin/verifier \
  build/bisheng_local/smoke5.bin \
  build/bisheng_local/smoke5.out

cat build/bisheng_local/smoke5.time
```

已验证的结果：

```text
case=0 n=128 b=8   status=PASS
case=1 n=128 b=16  status=PASS
case=2 n=128 b=32  status=PASS
case=3 n=128 b=64  status=PASS
case=4 n=128 b=128 status=PASS
```

## 7. 完整公开集验证

公开规格文件：

```text
contestant_sdk/cases/preliminary_public_150.txt
```

生成完整公开输入：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng/contestant_sdk

SPD_GENERATOR_THREADS=8 \
  ./build/bisheng_local/bin/spd_generator \
  build/bisheng_local/public_preliminary.bin \
  --spec cases/preliminary_public_150.txt
```

运行目标程序：

```bash
COMPILER2026_TIMING_FILE=build/bisheng_local/public_preliminary.time \
  ./build/bisheng_local/bin/baseline_serial \
  build/bisheng_local/public_preliminary.bin \
  build/bisheng_local/public_preliminary.out
```

校验：

```bash
VERIFIER_THREADS=8 \
  ./build/bisheng_local/bin/verifier \
  build/bisheng_local/public_preliminary.bin \
  build/bisheng_local/public_preliminary.out
```

后续你的并行程序应复用同样的输入输出格式和 verifier。

## 8. 分块 Cholesky 依赖关系

baseline 的核心循环：

```cpp
for (int i = 0; i < n; i += b) {
    cholesky(&L[i * n + i], &L[i * n + i], b, n);

    for (int j = i + b; j < n; j += b) {
        trsm(&L[j * n + i], &L[i * n + i], &L[j * n + i], b, n);
    }

    for (int j = i + b; j < n; j += b) {
        for (int k = j; k < n; k += b) {
            madd(&L[k * n + i], &L[j * n + i], &L[k * n + j], b, n);
        }
    }
}
```

依赖关系：

- `cholesky(i,i)` 是每一轮的起点。
- `trsm(j,i)` 依赖 `cholesky(i,i)`，不同 `j` 之间可并行。
- `madd(k,j,i)` 依赖 `trsm(k,i)` 和 `trsm(j,i)`。
- 下一轮的 `cholesky(i+b,i+b)` 依赖此前所有写入该对角块的 `madd`。

可抽象为 tile-level DAG：

```text
POTRF(i) -> TRSM(j,i) -> GEMM/MADD(k,j,i) -> POTRF(i+b)
```

本赛题中实际算子名为：

```c
void cholesky(double *A, double *L, int b, int lda);
void trsm(double *A, double *L, double *X, int b, int lda);
void madd(double *A, double *B, double *C, int b, int lda);
```

## 9. 推荐开发路线

第一阶段：保持功能正确。

- 构建 no-op Pass，确认官方模板链路稳定。
- 写最小 runtime，先只提供初始化、结束、串行调用包装。
- Pass 只识别目标函数和目标 call，不改变行为。

第二阶段：手工 DAG 并行化。

- 在 runtime 中实现线程池。
- 以块坐标作为任务 ID。
- 在 runtime 中根据 `n`、`b` 构造 Cholesky DAG。
- 任务执行时仍调用官方 `cholesky`、`trsm`、`madd`。
- Pass 负责把原始 `block_cholesky` 替换为 runtime 入口调用。

第三阶段：降低调度开销。

- 小 `b` 场景任务多，重点减少 atomic、锁和队列争用。
- 大 `b` 场景算子重，重点保证线程负载均衡。
- 可考虑分层队列、work stealing、按 wavefront 批量提交。

第四阶段：增强泛化能力。

- 不依赖固定公开 case。
- 不硬编码 `n`、`b`、seed、线程数或平台参数到源码标注中。
- 测试不同 `n`、不同 `b`、不同 case 数量。

## 10. Pass 开发要点

模板入口：

```text
contestant_sdk/template/llvm-pass/pass_stub.cpp
```

当前 Pass 名称：

```text
contestant-pass
```

常用 LLVM IR 关注点：

- 查找 `contest::block_cholesky` 函数。
- 查找 `cholesky`、`trsm`、`madd` 的 `CallBase`。
- 分析 `n`、`b` 参数来源。
- 插入 runtime 函数声明。
- 替换原始函数体或在关键位置插入 runtime 调用。

建议先做 module pass，避免过早引入复杂分析。能正确替换函数后，再考虑 LoopInfo、ScalarEvolution、DominatorTree 等分析。

## 11. Runtime 开发要点

模板入口：

```text
contestant_sdk/template/runtime/runtime_stub.cpp
```

建议 runtime API 初稿：

```c
extern "C" int compiler2026_block_cholesky_runtime(
    const double *A,
    double *L,
    int n,
    int b
);
```

runtime 内部可以：

- 复制 `A` 到 `L`。
- 构造任务图。
- 在线程池中调度任务。
- 调用官方 `cholesky`、`trsm`、`madd`。
- 等待所有任务完成。
- 清零上三角。

注意事项：

- runtime 可以链接 `-pthread`。
- 不要实现自己的 `cholesky`、`trsm`、`madd` 同名符号。
- 不要让任务并发写同一个 block。
- 出错时要能返回非 0 或安全失败，方便定位。

## 12. 正确性检查清单

每次改动后至少跑：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng/contestant_sdk

./build/bisheng_local/bin/verifier input.bin output.bin
```

重点关注：

- `case_count` 是否一致。
- 输出中的 `n`、`b` 是否与输入一致。
- `L` 是否为下三角结果，上三角可以为 0。
- `scaled_residual < 100`。
- 多线程执行是否存在偶发失败。

并行版本需要重复跑同一输入多次：

```bash
for i in $(seq 1 20); do
  ./your_program input.bin output.bin || exit 1
  ./build/bisheng_local/bin/verifier input.bin output.bin || exit 1
done
```

## 13. 性能检查清单

baseline 支持通过环境变量写出核心计算时间：

```bash
COMPILER2026_TIMING_FILE=some.time ./your_program input.bin output.bin
```

性能分析建议：

- 先记录串行 baseline 时间。
- 再记录并行版本时间。
- 分别观察小 `b`、中 `b`、大 `b`。
- 小矩阵不要过度追求加速，任务开销可能超过计算收益。
- 大矩阵重点提升 `madd` 并行度和线程利用率。

可用工具：

```bash
perf stat ./your_program input.bin output.bin
perf record -g ./your_program input.bin output.bin
perf report
```

虚拟机不是鲲鹏 920 真机，性能趋势只能作为参考。最终调优必须在真实目标平台复核。

## 14. 提交物结构

最小提交物由 `manifest.json` 描述：

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

当前模板构建后路径：

```text
build/template_bisheng/pass/libcontestant_pass.so
build/template_bisheng/runtime/libcontestant_runtime.a
build/template_bisheng/manifest.json
```

若使用源码标注，只允许使用：

```cpp
#pragma compiler2026 ...
[[clang::annotate("...")]]
__attribute__((annotate("...")))
```

标注后的源码副本应放在提交目录：

```text
src/baseline/main.cpp
src/baseline/block_cholesky.cpp
```

去掉标注后必须与官方 baseline 完全一致。

## 15. 常见问题

### 预编译工具不能运行

`contestant_sdk/bin/spd_generator` 和 `contestant_sdk/bin/verifier` 是 x86-64 Linux 产物，在 aarch64 VM 中不能运行。使用本文第 5 节从源码编译的版本。

### `libkernels.a` 不能链接

当前 SDK 中的 `contestant_sdk/lib/libkernels.a` 内部对象是 x86-64，不适合 aarch64 本地链接。开发时使用 `src/base_kernels/` 源码构建本地验证程序。正式评测会使用官方 judge 算子库。

### `opt` 加载 Pass 失败

常见原因是 Pass 用 LLVM 12 构建，却用毕昇 LLVM 15 的 `opt` 加载，或反过来。始终执行：

```bash
source /etc/profile.d/bisheng.sh
```

并在 CMake 中显式指定：

```bash
-DLLVM_CONFIG=/opt/bisheng/bin/llvm-config
```

### 本机 macOS 性能不可信

macOS ARM 只能用于编辑代码和轻量逻辑验证。功能、链接、Pass 插件加载和 Linux ABI 行为应在 openEuler VM 中验证。性能最终以鲲鹏 920 真机为准。

### `sample_matrices.bin` 格式不匹配

根目录 `sample_matrices.bin` 看起来是旧格式样例，不符合当前 SDK 文档中的 `case_count, n, b, A` 格式。当前开发优先使用 `spd_generator --spec cases/preliminary_public_150.txt` 生成输入。

## 16. 日常开发命令速查

登录 VM：

```bash
ssh -i ~/.ssh/bisheng_vm_ed25519 root@192.168.8.131
```

加载工具链：

```bash
source /etc/profile.d/bisheng.sh
```

进入项目：

```bash
cd /root/bisheng/contestant_sdk
```

重构建模板：

```bash
cmake --build build/template_bisheng -j$(nproc)
```

重构建本地验证程序：

```bash
clang++ -std=c++17 -O2 -pthread -Iinclude \
  tools/local_generator/spd_generator.cpp \
  -o build/bisheng_local/bin/spd_generator

clang++ -std=c++17 -O2 -pthread -Iinclude \
  tools/local_verifier/verifier.cpp \
  -o build/bisheng_local/bin/verifier

clang++ -std=c++17 -O2 -pthread \
  -Iinclude -Isrc/base_kernels \
  src/base_kernels/kernels_public.cpp \
  src/base_kernels/kernels_impl.cpp \
  src/baseline/main.cpp \
  src/baseline/block_cholesky.cpp \
  -o build/bisheng_local/bin/baseline_serial
```

跑 smoke test：

```bash
./build/bisheng_local/bin/spd_generator build/bisheng_local/smoke.bin 128 16 1
./build/bisheng_local/bin/baseline_serial build/bisheng_local/smoke.bin build/bisheng_local/smoke.out
./build/bisheng_local/bin/verifier build/bisheng_local/smoke.bin build/bisheng_local/smoke.out
```

同步本机代码到 VM：

```bash
rsync -az --delete \
  -e "ssh -i ~/.ssh/bisheng_vm_ed25519" \
  --exclude ".DS_Store" \
  --exclude "build" \
  /Users/chenzhongyuan/Documents/bisheng/ \
  root@192.168.8.131:/root/bisheng/
```

