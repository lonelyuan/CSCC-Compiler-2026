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
LABEL=runtime_submit_dequeue_batch REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

Enable runtime profiling for an async case:

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
SPEC_START=93 SPEC_END=93 COMPILER2026_DAG_THREADS=4 COMPILER2026_DAG_PROFILE=1 ./submission/scripts/smoke_test.sh
```

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
  calls into generated IR task functions, and inserts waits at loop exits.
- The runtime is a generic reusable task scheduler with arena context
  allocation, adaptive task submit/dequeue batching, and an opt-in profiling
  mode. Official `trsm` and `madd` ABI calls remain in Pass-generated IR task
  functions.
- No source annotations are required.

Documentation:

- Repository docs live under `../docs/`.
- Packaged archive docs are copied to `docs/` at archive root.
- `design.md`: implementation-level design.
- `performance.md`: benchmark results.
- `optimization_principles.md`: beginner-friendly explanation of
  parallelization and operator DAG scheduling.
- `roadmap.md`: longer-term scaling notes.
