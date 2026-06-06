# 设计说明

## 目标

本提交物通过 LLVM Pass 在 IR 层分析官方 baseline 中的算子调用和循环结构，将可并行的 `trsm`、`madd` 调用改写为运行时异步任务提交，并在对应 loop exit 插入同步点。官方 `cholesky`、`trsm`、`madd` 算子实现不被重定义、替换或绕过。

当前实现不再使用“直接清空 `block_cholesky` 函数体并替换成 runtime 入口”的方案。那个方案虽然能工作，但过于接近函数级替换，不符合本赛题强调的“LLVM Pass 分析算子依赖关系并插入运行时接口”的方向。

## Pass 设计

Pass 名称：

```text
contestant-pass
```

Pass 插件：

```text
pass/libcontestant_pass.so
```

Pass 类型：

```text
LLVM New Pass Manager FunctionPass
```

Pass 使用 `LoopAnalysis` 获取 `LoopInfo`，只处理官方 `contest::block_cholesky`：

- 函数名为官方 C++ mangled name `_ZN7contest14block_choleskyEPKdPdii`，或包含 `block_cholesky`。
- 返回类型为 `i32`。
- 参数数量为 4。

### IR 版本化

为了避免小 block 场景下任务提交开销超过计算收益，Pass 先克隆一份原始 IR 作为 serial fallback：

```text
compiler2026_serial_fallback(A, L, n, b)
```

然后在 `block_cholesky` 入口插入版本化分支：

```text
if (b < 64)
    return compiler2026_serial_fallback(A, L, n, b);
else
    run optimized IR path;
```

fallback 是从官方 baseline IR 克隆出来的，不是手写算法替换。

### 算子调用转换

在 optimized path 中：

- `cholesky(...)` 保持同步原始调用。
- `trsm(...)` 被 outline 到 Pass 生成的 IR task function：

```llvm
define internal void @compiler2026_task_trsm(ptr %ctx) {
  ...
  call void @trsm(ptr %A, ptr %L, ptr %X, i32 %b, i32 %lda)
  ret void
}
```

- `madd(...)` 被 outline 到 Pass 生成的 IR task function：

```llvm
define internal void @compiler2026_task_madd(ptr %ctx) {
  ...
  call void @madd(ptr %A, ptr %B, ptr %C, i32 %b, i32 %lda)
  ret void
}
```

- 原始 call site 分配参数上下文并提交 task function：

```llvm
%ctx = call ptr @compiler2026_runtime_alloc(...)
...
call void @compiler2026_runtime_submit(ptr @compiler2026_task_trsm, ptr %ctx)
```

Pass 根据 `LoopInfo` 插入同步：

- `trsm` 所在 loop 的 exit block 插入 `compiler2026_runtime_wait()`。
- `madd` 所在内层 loop 的父 loop exit block 插入 `compiler2026_runtime_wait()`，使同一 panel 的 `madd` 任务完成后再进入下一 panel。
- 函数 optimized path 入口插入 `compiler2026_runtime_begin(n, b)`。
- optimized path 返回前插入 `compiler2026_runtime_end()`。

优化后的 IR 仍保留原始 `block_cholesky` 的循环结构。检查命令：

```bash
llvm-dis build/optimization_benchmarks/ir/app.opt.bc -o - \
  | grep -n "compiler2026_task_\\|compiler2026_runtime_submit\\|call.*@trsm\\|call.*@madd\\|define.*block_cholesky\\|compiler2026_serial_fallback"
```

已验证 IR 片段包含：

```text
define dso_local noundef i32 @_ZN7contest14block_choleskyEPKdPdii(...)
  call i32 @compiler2026_serial_fallback(...)
  call void @compiler2026_runtime_submit(ptr @compiler2026_task_trsm, ptr ...)
  call void @compiler2026_runtime_submit(ptr @compiler2026_task_madd, ptr ...)

define internal void @compiler2026_task_trsm(ptr ...)
  call void @trsm(...)

define internal void @compiler2026_task_madd(ptr ...)
  call void @madd(...)

define internal noundef i32 @compiler2026_serial_fallback(...)
  tail call void @trsm(...)
  tail call void @madd(...)
```

## Runtime 设计

Runtime 静态库：

```text
runtime/libcontestant_runtime.a
```

Runtime API：

```c
extern "C" void compiler2026_runtime_begin(int n, int b);
extern "C" void *compiler2026_runtime_alloc(std::size_t size);
extern "C" void compiler2026_runtime_submit(void (*fn)(void *), void *ctx);
extern "C" void compiler2026_runtime_wait();
extern "C" void compiler2026_runtime_end();
```

Runtime 内部维护一个 thread-local `AsyncRuntime`：

- `runtime_alloc` 为 Pass 生成的 task context 分配内存。
- `runtime_submit` 只接收 task function 指针和 context 指针。
- worker 线程从队列中取任务，调用 Pass 生成的 task function，并释放 context。
- `wait` 等待队列为空且所有运行中任务完成。
- `end` 做最终等待并释放运行时上下文。

Runtime 不包含 `trsm` / `madd` 专用 wrapper，也不直接封装具体算子语义。官方 ABI 调用保留在 Pass 生成的 IR task function 中。`cholesky` 由优化后的 IR 保持原始同步调用。

## 正确性保证

分块 Cholesky 的依赖关系：

- `trsm(row, panel)` 依赖当前 panel 的 `cholesky(panel, panel)`。
- `madd(row, col, panel)` 依赖对应的 `trsm(row, panel)` 和 `trsm(col, panel)`。
- 下一 panel 的 `cholesky` 依赖前一 panel 的相关 `madd` 更新完成。

Pass 插入的 wait 保证：

- 所有 `trsm` 任务完成后才进入 `madd` 阶段。
- 所有 `madd` 任务完成后才进入下一 panel。

该策略是保守的 panel-barrier DAG，正确性优先。后续可以继续把 barrier 细化为真正的 ready-queue DAG。

## 当前限制

- 当前版本仍是 panel-barrier 调度，不是完全异步 DAG。
- 当前异步阈值 `b >= 64` 是 Pass 中的保守常量，后续可改成更系统的 profile-guided 或 runtime heuristic。
- 小 block fallback 能保证不因任务过细而严重退化，但当前 VM 上仍有少量版本化分支/函数调用开销。

## 本地验证

在 openEuler/BiSheng VM 中执行：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh
```

完整 benchmark：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
LABEL=ir_outlined_task_pass REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

详细性能记录见 `docs/performance.md`。
