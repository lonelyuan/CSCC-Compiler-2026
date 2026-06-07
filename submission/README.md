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
LABEL=runtime_ready_queue_trsm_deps REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

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
`COMPILER2026_ASYNC_MIN_B` / `COMPILER2026_ASYNC_MIN_BLOCKS` through to the
contestant binary, so small verifier runs can exercise the same runtime knobs
as benchmarks.

When the same profile flag is used with `benchmark.sh`, parsed profile metrics
are written into the benchmark CSV next to the timing fields:

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
COMPILER2026_DAG_PROFILE=1 LABEL=ready_queue_profile_csv_smoke REPEAT=1 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

For thread sweeps, benchmark output directories are split by
`threads_<count>` and the terminal summary groups suite/profile statistics by
the `threads` CSV column.

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
- It clones an async implementation for `b >= 32`, keeps `cholesky`
  synchronous, outlines async-path `trsm` and `madd`
  calls into generated IR task functions, recovers block coordinates from
  direct or nested one-dimensional GEP offsets, converts them to the runtime's
  current linear dependency keys, and inserts dependency-aware runtime submits
  plus panel-end waits.
- The runtime is a generic reusable task scheduler with arena context
  allocation, adaptive task submit/dequeue batching, an opt-in profiling mode,
  and a panel-local ready queue for `trsm` to `madd` dependencies. Official
  `trsm` and `madd` ABI calls remain in Pass-generated IR task functions.
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
