---
name: compiler-contest-assistant
description: >-
  Project assistant for the Bisheng/LLVM compiler contest workspace at
  /Users/chenzhongyuan/Documents/bisheng. Use when Codex works on this dynamic
  operator graph compilation and parallel scheduling project: configuring the
  authorized 40-core openEuler AArch64 cloud desktop or the 40-core Ubuntu
  Xeon scheduler host, syncing remote workspaces, building LLVM Pass/runtime artifacts,
  packaging/submitting zip deliverables, interpreting judge results, maintaining
  docs, benchmarking, or implementing compiler-level optimizations under contest
  rules without SDK hacks or fake speedups.
---

# Compiler Contest Assistant

Use this skill as the standing project guide for `/Users/chenzhongyuan/Documents/bisheng`. Prefer engineering changes that improve the compiler pass and runtime design, not one-off contest hacks.

## Workspace And Remote Hosts

- Local workspace: `/Users/chenzhongyuan/Documents/bisheng`.
- Treat the former 4-vCPU AArch64 VM as retired. Do not use it as a
  compatibility gate or put its private address/key path into new instructions.
  Leave its historical scripts and benchmark evidence untouched for pending
  branch integration, but do not select them for new runs.

### 40-core CourseGrading AArch64 cloud desktop

- The project has confirmed with the competition committee that it may establish
  its own private remote-access path. Do not generalize this permission to other
  teams or contests.
- The platform-native entry is the authenticated noVNC page. After bootstrap,
  connect by MagicDNS hostname with `scripts/cloud_desktop_ssh.sh` or
  `ssh root@bisheng-cg-aarch64`.
- Environment observed on 2026-08-05: openEuler 22.03 LTS, aarch64, 40 online
  CPUs, one thread per core, one NUMA node, and 75 GiB RAM.
- There is no `/dev/net/tun`. The verified topology is userspace `tailscaled`,
  private Tailnet TCP Serve port 22, and a public-key-only `sshd` listening on
  `127.0.0.1:2222`. Built-in Tailscale SSH did not return a server banner.
- Keep all Tailscale state, host keys, and logs outside the repository under
  `/mnt/cgshare/tailscale-cloud`. Never persist login URLs, auth keys, Tailnet
  addresses, node state, personal public keys, or private keys in Git.
- The base image does not include CMake, Ninja, or BiSheng. Provision the full
  AArch64 toolchain on persistent storage and expose it at `/opt/bisheng` before
  using the repository scripts.
- Full bootstrap, recovery, diagnosis, and teardown instructions live in
  `docs/cloud_desktop_remote_access.md`.
- This is the best available AArch64 performance-correlation candidate, but it
  is not formal judge evidence until a same-source, same-input, same-timing
  comparison has been completed. Calibrate against the observed formal judge
  result: 150/150 audit and perf passes, equal-weight per-case geomean
  `2.393844`, `m_ideal=32.0`.

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
- Use this host only for x86_64 scheduler scaling and same-host speedup comparisons. Its 40 physical cores approximate the judge's core-count scale, but its Ubuntu/x86_64/LLVM 17 stack does not provide BiSheng, AArch64, openEuler, or formal performance equivalence.
- Coordinate before replacing `~/bisheng`: another worktree or agent may have an active experiment there. `~/tb.sh` is a Round 7 task-batch sweep, not host bootstrap; do not overwrite or run it unless the current task calls for that experiment.
- Do not persist private keys, passwords, or authentication material in the repository. If key authentication fails, ask the user.

## Current Project Layout

- `submission/pass/dag_pass.cpp`: LLVM New PM pass. It clones an async version of `block_cholesky`, identifies official `trsm/madd` calls in IR, outlines task functions, recovers first-pass block keys from GEP offsets, and inserts runtime submit/wait or dependency-aware submit calls.
- `submission/runtime/dag_runtime.cpp`: generic task runtime with worker pool, ready queue, panel-local DAG dependencies, arena task-context allocation, optional profiling, and main-thread participation during wait.
- `submission/scripts/build.sh`: build pass/runtime.
- `submission/scripts/smoke_test.sh`: correctness verifier for a selected case range.
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
- Use the authorized 40-core AArch64 cloud desktop as the only active AArch64
  build/verifier/correlation environment. Use the Xeon host only for x86_64
  scheduler scaling. Treat old 4-vCPU VM numbers as historical evidence only.
- Design for real Kunpeng 920 multi-core scaling: likely 48/64-core class servers and possible larger final cases.
- When an optimization regresses or is not clearly valid, revert or document it as an experiment.

## Standard Commands

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

### 40-core CourseGrading AArch64 cloud desktop

After the noVNC bootstrap and toolchain provisioning documented in
`docs/cloud_desktop_remote_access.md`:

```bash
./scripts/cloud_desktop_ssh.sh --check
./scripts/sync_to_cloud_desktop.sh
./scripts/cloud_desktop_ssh.sh \
  'cd /mnt/cgshare/bisheng && ./submission/scripts/build.sh'
```

Run verifier and benchmark commands on `/mnt/cgshare`, recording the exact
source revision, command, CPU affinity, toolchain version, and output CSV. Do
not transfer files concurrently with timed runs, and do not compare absolute
times across AArch64 and Xeon hosts.

Use the following order for a cloud calibration run:

1. Record local `git rev-parse HEAD` and `git status --short`; do not describe a
   dirty source tree using only its commit ID.
2. Run `cloud_desktop_ssh.sh --check`, then sync source once. Stop all sync and
   downloads before timing.
3. Capture `uname -a`, `lscpu`, `nproc`, memory, governor/affinity where
   available, BiSheng/LLVM/CMake/Ninja versions, and a hash of the case spec.
4. Build, run `SPEC_START=1 SPEC_END=150` with the verifier, then run the
   detailed benchmark with `REPEAT>=3` and an explicit thread list.
5. Copy CSV/log/metadata artifacts back under `docs/benchmark_results/` using a
   label that identifies the host, source state, and date.
6. Compare with the formal `2.393844x` only after confirming that the runner
   computes an equal-weight geometric mean over the same individual cases.
   The current four-suite `benchmark.sh` summary and full-range
   `smoke_test.sh` aggregate are useful diagnostics but are not by themselves
   judge-equivalent per-case evidence.

## Submission/Judge Workflow

When preparing a submission:

1. Check and sync to the AArch64 cloud desktop.
2. Build.
3. Run smoke test with verifier.
4. Run a benchmark label and archive CSV under `docs/benchmark_results/` when the result matters.
5. Package.
6. Verify zip layout in a clean directory on the cloud desktop:

```bash
rm -rf /tmp/judge_zip_test
mkdir -p /tmp/judge_zip_test/submission /tmp/judge_zip_test/build
cd /tmp/judge_zip_test/submission
unzip -q /mnt/cgshare/bisheng/dist/submission.zip
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
