# Compiler 2026 Submission

This directory contains the contest-facing submission project.

Artifacts:

- `pass/libcontestant_pass.so`: LLVM new pass manager plugin.
- `runtime/libcontestant_runtime.a`: static runtime library.
- `manifest.json`: judge manifest.

Build on the openEuler/BiSheng VM:

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
./submission/scripts/build.sh
```

Run local smoke test:

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh
```

Run benchmark suites and write CSV results:

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
LABEL=live_window_default_repeat3_final REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

The current default auto batch uses an upper bound of `8` for `b <= 64`;
`COMPILER2026_TASK_BATCH` can still override it for platform tuning.

Sweep multiple runtime thread counts in one CSV:

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
COMPILER2026_DAG_THREAD_LIST=1,2,4 LABEL=thread_sweep REPEAT=1 ./submission/scripts/benchmark.sh
```

Enable runtime profiling for an async case:

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
SPEC_START=93 SPEC_END=93 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/smoke_test.sh
```

`smoke_test.sh` passes `COMPILER2026_DAG_THREADS`,
`COMPILER2026_DAG_PROFILE`, `COMPILER2026_TASK_BATCH`, and
`COMPILER2026_ASYNC_MIN_B` / `COMPILER2026_ASYNC_MIN_BLOCKS`,
`COMPILER2026_DAG_MAX_LIVE`, and `COMPILER2026_DAG_PIN_WORKERS` through to the
contestant binary, so small verifier runs can exercise the same runtime knobs
as benchmarks.

When the same profile flag is used with `benchmark.sh`, parsed profile metrics
are written into the benchmark CSV next to the timing fields. Cross-panel
experiments also report `dag_first_touch_deps`, so expected first touches of
original input blocks can be separated from the broader `dag_missing_deps`
counter:

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
COMPILER2026_DAG_PROFILE=1 LABEL=ready_queue_profile_csv_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

For thread sweeps, benchmark output directories are split by
`threads_<count>` and the terminal summary groups suite/profile statistics by
the `threads` CSV column. The terminal summary also reports speedup P50/P95 for
each suite and overall group. After a successful run, `benchmark.sh` removes
the large per-suite input/output/profile directories by default and keeps the
CSV, IR, and binaries. Set `COMPILER2026_BENCH_KEEP_ARTIFACTS=1` when you need
the per-run files for debugging.

Run an offline parameter sweep for target-platform tuning:

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
COMPILER2026_TUNE_THREAD_LIST=1,2,4,8 \
COMPILER2026_TUNE_ASYNC_MIN_B_LIST=18,24,32,48 \
COMPILER2026_TUNE_ASYNC_MIN_BLOCKS_LIST=2 \
COMPILER2026_TUNE_TASK_BATCH_LIST=auto,4,8 \
COMPILER2026_TUNE_DAG_MAX_LIVE_LIST=0 \
COMPILER2026_TUNE_DAG_PIN_WORKERS_LIST=0,1 \
COMPILER2026_TUNE_LABEL_PREFIX=target_param_sweep \
REPEAT=1 ./submission/scripts/tune_params.sh
```

The tuning wrapper calls `benchmark.sh` for each threshold/min-block/batch/live
window/pinning combination, lets `benchmark.sh` sweep the requested thread
list, and appends all rows to
`build/optimization_benchmarks/<label>_aggregate.csv`. It is intended for
pre-submission profiling on the real target machine, not for timed contestant
execution. It inherits the default artifact cleanup from `benchmark.sh`, so the
aggregate and per-combination CSV files remain while bulky suite directories
are removed after successful combinations.

An experimental cross-panel DAG path is available for development builds:

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
COMPILER2026_ENABLE_CROSS_PANEL_DAG=1 LABEL=cross_panel_profile REPEAT=1 \
  COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/benchmark.sh
```

This path taskizes `cholesky`, adds a three-dependency submit for `madd`, and
replaces the static panel-end wait with one outer DAG drain wait. It currently
passes local verifier smoke but is slower than the default panel-local DAG on
the 4-vCPU VM, so it is opt-in.

For the same experimental path, `COMPILER2026_DAG_MAX_LIVE=<N>` enables a
generic submit-side live-window drain. When live DAG pressure exceeds the
window and ready tasks exist, the submitting thread runs a small ready batch
before continuing. The default is `0`, so the current panel-local path is not
changed unless the variable is set.

Set `COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1` together with
`COMPILER2026_ENABLE_CROSS_PANEL_DAG=1` to use the newer experimental lowering:
`cholesky` stays as the original synchronous ABI call, while the pass inserts a
generic `runtime_wait_key(block)` before each panel factorization and continues
to submit `trsm/madd` into the cross-panel DAG. This reduces extra cholesky
tasks and remains opt-in until verifier and repeat benchmark data justify a
default change.

`COMPILER2026_DAG_PIN_WORKERS=1` is a Linux-only opt-in runtime experiment that
pins worker threads to CPUs from the current process affinity mask. It is
intended for target-platform NUMA/affinity testing and is off by default.

Package artifacts:

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
./submission/scripts/package.sh
```

The package script writes both:

- `dist/submission.tar.gz`
- `dist/submission.zip`

Both archives place `CMakeLists.txt` at archive root so the judge can run CMake
directly on the extracted submission directory.

Current implementation:

- The pass analyzes the official `contest::block_cholesky` IR with LoopInfo.
- It keeps the original function body as the small-block serial path.
- It clones an async implementation for `b >= 18`, keeps `cholesky`
  synchronous, outlines async-path `trsm` and `madd`
  calls into generated IR task functions, recovers block coordinates from
  direct or nested one-dimensional GEP offsets, converts them to the runtime's
  current linear dependency keys, and inserts dependency-aware runtime submits
  plus panel-end waits. Because the current DAG scope still ends at the panel
  wait, `madd` submit calls use output key `-1`; `trsm` output keys remain in
  the runtime producer table and release dependent `madd` tasks.
- The runtime is a generic reusable task scheduler with arena context
  allocation, adaptive task submit/dequeue batching, an opt-in profiling mode,
  a panel-local ready queue for `trsm` to `madd` dependencies, and a contiguous
  successor edge pool for DAG fanout. Official `trsm` and `madd` ABI calls
  remain in Pass-generated IR task functions.
- `COMPILER2026_ENABLE_CROSS_PANEL_DAG=1` enables an experimental full-DAG
  lowering where Pass-generated `cholesky`, `trsm`, and `madd` task functions
  directly call the official ABI operators. The default remains panel-local
  because current VM evidence shows lower overhead and better geomean speedup.
- `COMPILER2026_CROSS_PANEL_SYNC_CHOLESKY=1` changes that experimental lowering
  to keep `cholesky` synchronous and wait only for the needed dependency key
  before the call. The runtime interface is still generic and only sees integer
  block keys; key waiters sleep on completion notifications instead of polling
  on a fixed timeout.
- `COMPILER2026_DAG_MAX_LIVE` is an opt-in live-window control for that
  experimental path; it is intended for target-platform tuning, not as a new
  default.
- `COMPILER2026_DAG_PIN_WORKERS=1` optionally pins runtime worker threads on
  Linux. The runtime reads the process affinity mask and ignores the request if
  pinning is unavailable.
- `scripts/tune_params.sh` provides an offline wrapper around `benchmark.sh` for
  environment-specific threshold/batch/thread sweeps. Runtime defaults remain
  deterministic and cheap during contestant execution.
- No source annotations are required.

Documentation:

- Repository docs live under `../docs/`.
- Packaged archive docs are copied to `docs/` at archive root.
- `design.md`: implementation-level design.
- `performance.md`: benchmark results.
- `optimization_principles.md`: beginner-friendly explanation of
  parallelization and operator DAG scheduling.
- `roadmap.md`: longer-term scaling notes.
- `engineering_log.md`: verified lessons and constraints from each
  optimization step.
- `technical_scheme_notes.md`: extracted contest-rule notes from the official
  technical scheme PDF.
