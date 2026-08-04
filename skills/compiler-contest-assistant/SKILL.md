---
name: compiler-contest-assistant
description: >-
  Project assistant for the Bisheng/LLVM compiler contest workspace at
  /Users/chenzhongyuan/Documents/bisheng. Use when Codex works on this dynamic
  operator graph compilation and parallel scheduling project: configuring the
  openEuler/BiSheng compatibility VM or 40-core Ubuntu Xeon performance host,
  syncing remote workspaces, building LLVM Pass/runtime artifacts,
  packaging/submitting zip deliverables, interpreting judge results, maintaining
  docs, benchmarking, or implementing compiler-level optimizations under contest
  rules without SDK hacks or fake speedups.
---

# Compiler Contest Assistant

Use this skill as the standing project guide for `/Users/chenzhongyuan/Documents/bisheng`. Prefer engineering changes that improve the compiler pass and runtime design, not one-off contest hacks.

## Workspace And Remote Hosts

- Local workspace: `/Users/chenzhongyuan/Documents/bisheng`.

### openEuler/BiSheng compatibility VM

- VM project path: `/root/bisheng`.
- VM login: `ssh -i ~/.ssh/bisheng_vm_ed25519 -o StrictHostKeyChecking=no root@192.168.8.131`.
- VM environment: openEuler aarch64, BiSheng under `/opt/bisheng`, LLVM 15.0.4.
- Always load toolchain in VM commands:

```bash
source /etc/profile.d/bisheng.sh
```

- Sync local project to VM with:

```bash
./scripts/sync_to_vm.sh
```

- Do not hard-code or persist plaintext passwords in files. Use the existing SSH key path above; if authentication fails, ask the user.

### 40-core Ubuntu Xeon performance host

- Login: `ssh -i ~/.ssh/ouc_xeon_ed25519 -p 6000 -o StrictHostKeyChecking=accept-new ouc@43.142.45.204`.
- Remote project mirror: `/home/ouc/bisheng` (`~/bisheng`). It has no Git metadata; keep the local repository as the source of truth.
- Environment: Ubuntu 22.04.5 x86_64, two Intel Xeon Gold 5218R CPUs, 40 physical cores / 80 logical CPUs, LLVM 17.0.6, no BiSheng toolchain.
- Load the LLVM 17 environment before every build or experiment:

```bash
source ~/llvm17.env
```

- `~/llvm17.env` selects `/usr/lib/llvm-17`, sets generator/verifier concurrency to 40, and preserves benchmark artifacts.
- Use `taskset -c 0-39` for the established 40-physical-core experiment convention. Record any different CPU affinity in the benchmark evidence.
- Use this host for x86_64 scheduler scaling and same-host speedup comparisons. Do not treat it as BiSheng/aarch64 compatibility proof or as the formal judge performance environment.
- Coordinate before replacing `~/bisheng`: another worktree or agent may have an active experiment there. `~/tb.sh` is a Round 7 task-batch sweep, not host bootstrap; do not overwrite or run it unless the current task calls for that experiment.
- Do not persist private keys, passwords, or authentication material in the repository. If key authentication fails, ask the user.

## Current Project Layout

- `submission/pass/dag_pass.cpp`: LLVM New PM pass. It clones an async version of `block_cholesky`, identifies official `trsm/madd` calls in IR, outlines task functions, recovers first-pass block keys from GEP offsets, and inserts runtime submit/wait or dependency-aware submit calls.
- `submission/runtime/dag_runtime.cpp`: generic task runtime with worker pool, ready queue, panel-local DAG dependencies, arena task-context allocation, optional profiling, and main-thread participation during wait.
- `submission/scripts/build.sh`: build pass/runtime.
- `submission/scripts/smoke_test.sh`: local VM correctness smoke test.
- `submission/scripts/benchmark.sh`: benchmark selected public case ranges and write CSV.
- `submission/scripts/package.sh`: build and create `dist/submission.zip` and `.tar.gz`; zip root must contain `CMakeLists.txt`.
- `docs/`: project docs, benchmark CSVs, design notes, principle explanation, and roadmap.
- `contestant_sdk/`: official SDK. Treat as read-only unless the user explicitly asks to inspect or regenerate local test artifacts.

## Competition Constraints

Follow these rules strictly:

- Optimize by adding LLVM Passes and optional runtime support.
- Do not replace the whole official algorithm with a hand-written Cholesky implementation.
- Do not modify official baseline source except allowed annotations; current project does not require annotations.
- Do not modify, redefine, bypass, or fake official `cholesky`, `trsm`, or `madd` implementations.
- Keep official ABI calls visible in Pass-generated IR task functions, e.g. `compiler2026_task_trsm` calls `@trsm`, and `compiler2026_task_madd` calls `@madd`.
- Runtime must be generic scheduling support. It should submit and execute task functions, not contain operator-specific wrappers that hide official ABI calls.
- Do not hack SDK tools, public cases, timing, verifier, generator, file I/O, or result output.
- Do not claim performance results without a reproducible CSV, command, and correctness verifier pass.

## Official Technical Scheme Notes

The official PDF in `docs/2026年全国大学生计算机系统能力大赛编译系统设计赛-编译系统挑战赛-动态算子图编译与并行调度-技术方案.pdf` is summarized in `docs/technical_scheme_notes.md`. Use it as the contest-rule source before making strategic changes.

- The task is explicitly "dynamic operator graph compilation and parallel scheduling": analyze operator dependencies in LLVM, generate an executable, preserve numerical correctness, and maximize parallel performance.
- The target workload is blocked Cholesky. The official operators are `cholesky`, `trsm`, and `madd`; the intended optimization is dependency-aware parallel execution of these operators.
- Source annotations are allowed only as analysis aids and must be documented. The current project does not depend on annotations.
- Runtime support is allowed, but its API and semantics must be documented. It should remain generic scheduling infrastructure.
- Performance timing is for the core computation only, after correctness filtering; file I/O and verification are excluded by the judge.
- Functional pass rate has a 90% gate. Below the gate, performance score is not counted. Full correctness is strategically mandatory.
- Performance score is based on geometric mean speedup, normalized by an ideal factor such as `m_ideal=128`: roughly `performance_score = 100 * geo_speedup / m_ideal`. A `2x` geomean speedup therefore yields only about `1.56` performance points when `m_ideal=128`.
- Initial scoring weights are 40% functional pass rate and 60% performance score. With full correctness and `m_ideal=128`, total score is roughly `40 + 0.6 * (100 * geo_speedup / 128)`.
- Finals add 50 hidden matrix cases and include non-code scoring: objective score 30%, design document 20%, teamwork/defense 50%. Keep design docs, benchmark evidence, bottlenecks, and comparisons current.
- Test matrices use double matrices, widths `n` from 3 to 10000, row-major binary input/output, and per-matrix block size `b`.

Official blocked Cholesky dependencies to preserve and exploit:

```text
cholesky(panel) produces the current diagonal block
trsm(row, panel) depends on cholesky(panel)
madd(row, col, panel) depends on trsm(row, panel) and trsm(col, panel)
cholesky(panel + 1) only needs updates to block (panel + 1, panel + 1), not every trailing update
trsm(row, panel + 1) only needs updates to block (row, panel + 1)
```

Strategic implication: after the current panel-local DAG, the largest rule-compliant performance space is cross-panel dependency recovery and scheduling, not more profiling counters or source/operator rewrites.

## Engineering Posture

Prioritize compiler credibility:

- Read existing code and docs before changing behavior.
- Keep changes scoped and reversible; use git commits for meaningful milestones.
- Preserve judge-compatible package layout: archive root must contain `CMakeLists.txt`.
- Prefer IR analysis and dependency recovery over source rewriting.
- Treat the 4-vCPU openEuler VM as the BiSheng compatibility gate and the 40-core Xeon host as a scalability/performance debugging platform; neither substitutes for formal judge results.
- Design for real Kunpeng 920 multi-core scaling: likely 48/64-core class servers and possible larger final cases.
- When an optimization regresses or is not clearly valid, revert or document it as an experiment.

## Standard Commands

### openEuler/BiSheng compatibility VM

Build submission on VM:

```bash
ssh -i ~/.ssh/bisheng_vm_ed25519 -o StrictHostKeyChecking=no root@192.168.8.131 \
  'source /etc/profile.d/bisheng.sh && cd /root/bisheng && ./submission/scripts/build.sh'
```

Smoke test selected case range:

```bash
ssh -i ~/.ssh/bisheng_vm_ed25519 -o StrictHostKeyChecking=no root@192.168.8.131 \
  'source /etc/profile.d/bisheng.sh && cd /root/bisheng && SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh'
```

Benchmark:

```bash
ssh -i ~/.ssh/bisheng_vm_ed25519 -o StrictHostKeyChecking=no root@192.168.8.131 \
  'source /etc/profile.d/bisheng.sh && cd /root/bisheng && LABEL=<label> REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh'
```

Package:

```bash
ssh -i ~/.ssh/bisheng_vm_ed25519 -o StrictHostKeyChecking=no root@192.168.8.131 \
  'source /etc/profile.d/bisheng.sh && cd /root/bisheng && ./submission/scripts/package.sh'
scp -i ~/.ssh/bisheng_vm_ed25519 -o StrictHostKeyChecking=no \
  root@192.168.8.131:/root/bisheng/dist/submission.zip dist/submission.zip
```

Inspect optimized IR for compliance:

```bash
ssh -i ~/.ssh/bisheng_vm_ed25519 -o StrictHostKeyChecking=no root@192.168.8.131 \
  'source /etc/profile.d/bisheng.sh && cd /root/bisheng && llvm-dis build/optimization_benchmarks/ir/app.opt.bc -o - 2>/dev/null | grep -n "compiler2026_async_impl\|compiler2026_task_\|compiler2026_runtime_submit\|call.*@trsm\|call.*@madd\|define.*block_cholesky" | sed -n "1,160p"'
```

### 40-core Ubuntu Xeon performance host

Build the current remote mirror:

```bash
ssh -i ~/.ssh/ouc_xeon_ed25519 -p 6000 -o StrictHostKeyChecking=accept-new ouc@43.142.45.204 \
  'source ~/llvm17.env && cd ~/bisheng && ./submission/scripts/build.sh'
```

Run a judge-shaped benchmark on the 40 physical cores:

```bash
ssh -i ~/.ssh/ouc_xeon_ed25519 -p 6000 -o StrictHostKeyChecking=accept-new ouc@43.142.45.204 \
  'source ~/llvm17.env && cd ~/bisheng && LABEL=<label> REPEAT=3 COMPILER2026_DAG_THREAD_LIST=40 taskset -c 0-39 ./submission/scripts/benchmark.sh'
```

Archive the resulting CSV under `docs/benchmark_results/` and record the exact remote commit/source state. Compare only against baselines measured on this same host and affinity configuration.

## Submission/Judge Workflow

When preparing a submission:

1. Sync to VM.
2. Build.
3. Run smoke test with verifier.
4. Run a benchmark label and archive CSV under `docs/benchmark_results/` when the result matters.
5. Package.
6. Verify zip layout locally or on VM:

```bash
rm -rf /tmp/judge_zip_test
mkdir -p /tmp/judge_zip_test/submission /tmp/judge_zip_test/build
cd /tmp/judge_zip_test/submission
unzip -q /root/bisheng/dist/submission.zip
cmake -S /tmp/judge_zip_test/submission -B /tmp/judge_zip_test/build -G Ninja \
  -DLLVM_CONFIG=/opt/bisheng/bin/llvm-config \
  -DCMAKE_C_COMPILER=/opt/bisheng/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/bisheng/bin/clang++
cmake --build /tmp/judge_zip_test/build -j"$(nproc)"
```

7. Commit code/docs changes.

For an authorized online upload, switch to the repository's
`contest-submission` skill. Keep formal judge submission separate from the
cloud-desktop evaluation workflow.

If judge reports CE, first inspect package root layout, `CMakeLists.txt`, `manifest.json`, CMake output, and whether paths assume repository root instead of submission root.

## Optimization Roadmap

Current implementation is a panel-local ready-queue DAG:

```text
cholesky(panel) -> trsm(panel) tasks
madd(row,col,panel) depends on trsm(row,panel), trsm(col,panel)
panel end waits before next panel
```

Near-term improvements:

- Strengthen block-coordinate recovery beyond the current direct GEP-offset key.
- Extend explicit dependencies across panel boundaries for `cholesky(panel+1)` and `trsm(row,panel+1)`.
- Replace the remaining panel-end wait with ready-queue DAG scheduling where safe.
- Use existing DAG profiling counters for dependency edges, ready releases, queue depth, and critical-path symptoms to guide the next scheduler changes.
- Make task granularity adaptive to `n`, `b`, block count, and thread count.

Long-term improvements:

- Per-worker queues and work stealing instead of one global queue.
- NUMA-aware scheduling and worker pinning on real Kunpeng servers.
- Profile-guided thresholds instead of hard-coded VM-tuned constants.
- Compare with PLASMA/QUARK, StarPU, PaRSEC, OpenMP task depend lowering, LLVM Polly/MLIR affine analysis, and Tapir/Cilk-style task IR.

## Response Style For This Project

- Say exactly what was changed, where results are stored, and what was verified.
- Give paths using local file links when reporting changed files.
- Distinguish “verified performance improvement” from “experiment”.
- If blocked, recommend concrete research or implementation directions rather than proposing rule-breaking shortcuts.
