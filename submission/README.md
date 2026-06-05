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
LABEL=after_madd_coarsening REPEAT=3 COMPILER2026_DAG_THREADS=4 ./submission/scripts/benchmark.sh
```

Package artifacts:

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
./submission/scripts/package.sh
```

Current implementation:

- The pass replaces the official `contest::block_cholesky` body with a call to
  `compiler2026_block_cholesky_runtime`.
- The runtime preserves the official algorithm semantics, calls the official
  `cholesky`, `trsm`, and `madd` ABI, and schedules tile-level work with panel
  barriers.
- No source annotations are required.
