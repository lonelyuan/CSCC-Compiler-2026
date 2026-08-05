#!/usr/bin/env bash
# Build and sweep the small-b coarsening feasibility probe (tools/coarsen_feasibility).
# Reports, per (n, b), what a hand-written coarsened panel-barrier schedule achieves
# against the SDK's serial loop -- an upper bound for the Pass-side range task.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SDK_DIR="${REPO_ROOT}/contestant_sdk"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/build/coarsen_probe}"
CXX_BIN="${CXX:-/opt/bisheng/bin/clang++}"
THREADS="${THREADS:-40}"
REPEATS="${REPEATS:-3}"
TASKSET_CPUS="${TASKSET_CPUS:-0-39}"
# n:b pairs drawn from the public 150, covering the b<12 range that the async
# predicate currently excludes, plus b=12/16 as already-parallel controls.
CASES="${CASES:-1152:8 2048:8 1024:8 576:8 256:8 1152:9 1280:10 1152:12 1152:16}"

mkdir -p "${WORK_DIR}"
cd "${SDK_DIR}"
"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude -Isrc/base_kernels \
  src/base_kernels/kernels_public.cpp src/base_kernels/kernels_impl.cpp \
  "${REPO_ROOT}/tools/coarsen_feasibility/main.cpp" -o "${WORK_DIR}/coarsen_probe"

runner=()
if [[ -n "${TASKSET_CPUS}" ]] && command -v taskset >/dev/null 2>&1; then
  runner=(taskset -c "${TASKSET_CPUS}")
fi

for case_spec in ${CASES}; do
  n="${case_spec%%:*}"
  b="${case_spec##*:}"
  "${runner[@]}" "${WORK_DIR}/coarsen_probe" "${n}" "${b}" "${THREADS}" "${REPEATS}"
done
