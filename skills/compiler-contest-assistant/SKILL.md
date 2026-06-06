---
name: compiler-contest-assistant
description: >-
  Project assistant for the Bisheng/LLVM compiler contest workspace at
  /Users/chenzhongyuan/Documents/bisheng. Use when Codex works on this dynamic
  operator graph compilation and parallel scheduling project: configuring the
  openEuler/BiSheng VM, syncing to root@192.168.8.131, building LLVM
  Pass/runtime artifacts, packaging/submitting zip deliverables, interpreting
  judge results, maintaining docs, benchmarking, or implementing compiler-level
  optimizations under contest rules without SDK hacks or fake speedups.
---

# Compiler Contest Assistant

Use this skill as the standing project guide for `/Users/chenzhongyuan/Documents/bisheng`. Prefer engineering changes that improve the compiler pass and runtime design, not one-off contest hacks.

## Workspace And VM

- Local workspace: `/Users/chenzhongyuan/Documents/bisheng`.
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

## Engineering Posture

Prioritize compiler credibility:

- Read existing code and docs before changing behavior.
- Keep changes scoped and reversible; use git commits for meaningful milestones.
- Preserve judge-compatible package layout: archive root must contain `CMakeLists.txt`.
- Prefer IR analysis and dependency recovery over source rewriting.
- Treat the current 4-vCPU VM as a debugging platform, not the real performance target.
- Design for real Kunpeng 920 multi-core scaling: likely 48/64-core class servers and possible larger final cases.
- When an optimization regresses or is not clearly valid, revert or document it as an experiment.

## Standard Commands

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
