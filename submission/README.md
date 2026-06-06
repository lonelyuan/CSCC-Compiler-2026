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
LABEL=ir_outlined_task_pass REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

Package artifacts:

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
./submission/scripts/package.sh
```

Current implementation:

- The pass analyzes the official `contest::block_cholesky` IR with LoopInfo.
- It keeps `cholesky` synchronous, outlines optimized-path `trsm` and `madd`
  calls into generated IR task functions, and inserts waits at loop exits.
- It clones the original IR as a serial fallback for small block sizes.
- The runtime is a generic task scheduler. Official `trsm` and `madd` ABI calls
  remain in Pass-generated IR task functions.
- No source annotations are required.
