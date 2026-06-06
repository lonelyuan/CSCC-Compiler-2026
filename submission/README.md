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
LABEL=pass_runtime_threshold32 REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
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
  allocation. Official `trsm` and `madd` ABI calls remain in Pass-generated IR
  task functions.
- No source annotations are required.

Documentation:

- `docs/design.md`: implementation-level design.
- `docs/performance.md`: benchmark results.
- `docs/optimization_principles.md`: beginner-friendly explanation of
  parallelization and operator DAG scheduling.
- `docs/roadmap.md`: longer-term scaling notes.
