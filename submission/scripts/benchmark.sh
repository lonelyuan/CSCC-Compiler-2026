#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUBMISSION_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${SUBMISSION_DIR}/.." && pwd)"
SDK_DIR="${REPO_ROOT}/contestant_sdk"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/submission_bisheng}"
BENCH_DIR="${BENCH_DIR:-${REPO_ROOT}/build/optimization_benchmarks}"
CXX_BIN="${CXX:-/opt/bisheng/bin/clang++}"
CLANG_BIN="${CLANG:-/opt/bisheng/bin/clang++}"
OPT_BIN="${OPT:-/opt/bisheng/bin/opt}"
LLVM_LINK_BIN="${LLVM_LINK:-/opt/bisheng/bin/llvm-link}"
THREADS="${COMPILER2026_DAG_THREADS:-4}"
PROFILE="${COMPILER2026_DAG_PROFILE:-0}"
TASK_BATCH="${COMPILER2026_TASK_BATCH:-auto}"
ASYNC_MIN_B="${COMPILER2026_ASYNC_MIN_B:-32}"
REPEAT="${REPEAT:-3}"
LABEL="${LABEL:-run}"

"${SUBMISSION_DIR}/scripts/build.sh" >/dev/null

mkdir -p "${BENCH_DIR}/bin" "${BENCH_DIR}/ir" "${BENCH_DIR}/${LABEL}"
cd "${SDK_DIR}"

"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_generator/spd_generator.cpp \
  -o "${BENCH_DIR}/bin/spd_generator"

"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_verifier/verifier.cpp \
  -o "${BENCH_DIR}/bin/verifier"

"${CXX_BIN}" -std=c++17 -O2 -pthread \
  -Iinclude -Isrc/base_kernels \
  src/base_kernels/kernels_public.cpp \
  src/base_kernels/kernels_impl.cpp \
  src/baseline/main.cpp \
  src/baseline/block_cholesky.cpp \
  -o "${BENCH_DIR}/bin/baseline_serial"

"${CLANG_BIN}" -std=c++17 -O2 -Iinclude -Isrc/base_kernels \
  -emit-llvm -c src/baseline/main.cpp \
  -o "${BENCH_DIR}/ir/main.bc"

"${CLANG_BIN}" -std=c++17 -O2 -Iinclude -Isrc/base_kernels \
  -emit-llvm -c src/baseline/block_cholesky.cpp \
  -o "${BENCH_DIR}/ir/block_cholesky.bc"

"${LLVM_LINK_BIN}" \
  "${BENCH_DIR}/ir/main.bc" \
  "${BENCH_DIR}/ir/block_cholesky.bc" \
  -o "${BENCH_DIR}/ir/app.bc"

"${OPT_BIN}" \
  -load-pass-plugin "${BUILD_DIR}/pass/libcontestant_pass.so" \
  -passes=contestant-pass \
  "${BENCH_DIR}/ir/app.bc" \
  -o "${BENCH_DIR}/ir/app.opt.bc"

"${CXX_BIN}" -std=c++17 -O2 -pthread \
  -Iinclude -Isrc/base_kernels \
  "${BENCH_DIR}/ir/app.opt.bc" \
  src/base_kernels/kernels_public.cpp \
  src/base_kernels/kernels_impl.cpp \
  "${BUILD_DIR}/runtime/libcontestant_runtime.a" \
  -o "${BENCH_DIR}/bin/contestant_app"

CSV="${BENCH_DIR}/${LABEL}.csv"
echo "label,suite,repeat,threads,task_batch,async_min_b,profile_enabled,serial_seconds,contestant_seconds,speedup,profile_calls,total_tasks,main_tasks,worker_tasks,flushes,dequeue_batches,max_batch,max_ready,dag_nodes,dag_edges,dag_initial_ready,dag_released,max_dag_pending,queue_ms,exec_ms,worker_idle_ms,trsm_count,trsm_queue_ms,trsm_exec_ms,madd_count,madd_queue_ms,madd_exec_ms" > "${CSV}"

run_suite() {
  local suite="$1"
  local start="$2"
  local end="$3"
  local suite_dir="${BENCH_DIR}/${LABEL}/${suite}"
  mkdir -p "${suite_dir}"

  sed -n "${start},${end}p" cases/preliminary_public_150.txt > "${suite_dir}/cases.spec"

  SPD_GENERATOR_THREADS="${GENERATOR_THREADS:-4}" \
    "${BENCH_DIR}/bin/spd_generator" \
    "${suite_dir}/input.bin" \
    --spec "${suite_dir}/cases.spec"

  for run in $(seq 1 "${REPEAT}"); do
    COMPILER2026_TIMING_FILE="${suite_dir}/serial_${run}.time" \
      "${BENCH_DIR}/bin/baseline_serial" \
      "${suite_dir}/input.bin" \
      "${suite_dir}/serial_${run}.out"

    if ! COMPILER2026_TIMING_FILE="${suite_dir}/contestant_${run}.time" \
      COMPILER2026_DAG_THREADS="${THREADS}" \
      COMPILER2026_DAG_PROFILE="${PROFILE}" \
        "${BENCH_DIR}/bin/contestant_app" \
        "${suite_dir}/input.bin" \
        "${suite_dir}/contestant_${run}.out" \
        2> "${suite_dir}/contestant_${run}.profile"; then
      cat "${suite_dir}/contestant_${run}.profile" >&2
      exit 1
    fi

    if [[ "${run}" == "1" ]]; then
      VERIFIER_THREADS="${VERIFIER_THREADS:-4}" \
        "${BENCH_DIR}/bin/verifier" \
        "${suite_dir}/input.bin" \
        "${suite_dir}/contestant_${run}.out" > "${suite_dir}/contestant.verify"
    fi

    python3 - "${LABEL}" "${suite}" "${run}" "${THREADS}" "${TASK_BATCH}" \
      "${ASYNC_MIN_B}" "${PROFILE}" \
      "${suite_dir}/serial_${run}.time" \
      "${suite_dir}/contestant_${run}.time" \
      "${suite_dir}/contestant_${run}.profile" >> "${CSV}" <<'PY'
import csv
import re
import sys

(
    label,
    suite,
    run,
    threads,
    task_batch,
    async_min_b,
    profile_enabled,
    serial_path,
    contestant_path,
    profile_path,
) = sys.argv[1:]
serial = float(open(serial_path).read())
contestant = float(open(contestant_path).read())
speedup = serial / contestant if contestant > 0 else float("inf")

profile_calls = 0
summary_counts = {
    "tasks": 0,
    "main_tasks": 0,
    "worker_tasks": 0,
    "flushes": 0,
    "dequeue_batches": 0,
    "dag_nodes": 0,
    "dag_edges": 0,
    "dag_initial_ready": 0,
    "dag_released": 0,
}
summary_max = {
    "max_batch": 0,
    "max_ready": 0,
    "max_dag_pending": 0,
}
summary_ms = {
    "queue_ms": 0.0,
    "exec_ms": 0.0,
    "worker_idle_ms": 0.0,
}
tasks = {
    "trsm": {"count": 0, "queue_ms": 0.0, "exec_ms": 0.0},
    "madd": {"count": 0, "queue_ms": 0.0, "exec_ms": 0.0},
}
pair_re = re.compile(r"([A-Za-z_]+)=([^ ]+)")

try:
    lines = open(profile_path).read().splitlines()
except FileNotFoundError:
    lines = []

for line in lines:
    if line.startswith("[compiler2026_profile] "):
        profile_calls += 1
        values = dict(pair_re.findall(line))
        for key in summary_counts:
            summary_counts[key] += int(values.get(key, "0"))
        for key in summary_max:
            summary_max[key] = max(summary_max[key], int(values.get(key, "0")))
        for key in summary_ms:
            summary_ms[key] += float(values.get(key, "0"))
    elif line.startswith("[compiler2026_profile_task] "):
        values = dict(pair_re.findall(line))
        name = values.get("name")
        if name in tasks:
            tasks[name]["count"] += int(values.get("count", "0"))
            tasks[name]["queue_ms"] += float(values.get("queue_ms", "0"))
            tasks[name]["exec_ms"] += float(values.get("exec_ms", "0"))

writer = csv.writer(sys.stdout, lineterminator="\n")
writer.writerow([
    label,
    suite,
    run,
    threads,
    task_batch,
    async_min_b,
    "1" if profile_enabled not in ("", "0") else "0",
    f"{serial:.9f}",
    f"{contestant:.9f}",
    f"{speedup:.6f}",
    profile_calls,
    summary_counts["tasks"],
    summary_counts["main_tasks"],
    summary_counts["worker_tasks"],
    summary_counts["flushes"],
    summary_counts["dequeue_batches"],
    summary_max["max_batch"],
    summary_max["max_ready"],
    summary_counts["dag_nodes"],
    summary_counts["dag_edges"],
    summary_counts["dag_initial_ready"],
    summary_counts["dag_released"],
    summary_max["max_dag_pending"],
    f"{summary_ms['queue_ms']:.3f}",
    f"{summary_ms['exec_ms']:.3f}",
    f"{summary_ms['worker_idle_ms']:.3f}",
    tasks["trsm"]["count"],
    f"{tasks['trsm']['queue_ms']:.3f}",
    f"{tasks['trsm']['exec_ms']:.3f}",
    tasks["madd"]["count"],
    f"{tasks['madd']['queue_ms']:.3f}",
    f"{tasks['madd']['exec_ms']:.3f}",
])
PY
  done
}

run_suite "n512_576" 43 56
run_suite "n768" 71 81
run_suite "n1024" 91 96
run_suite "n1152_small_b" 97 104

python3 - "${CSV}" <<'PY'
import csv, statistics, sys
path = sys.argv[1]
rows = list(csv.DictReader(open(path)))
print(f"csv={path}")
for suite in sorted({r["suite"] for r in rows}):
    vals = [float(r["speedup"]) for r in rows if r["suite"] == suite]
    serial = [float(r["serial_seconds"]) for r in rows if r["suite"] == suite]
    contestant = [float(r["contestant_seconds"]) for r in rows if r["suite"] == suite]
    print(
        f"{suite}: serial_avg={statistics.mean(serial):.6f}s "
        f"contestant_avg={statistics.mean(contestant):.6f}s "
        f"speedup_avg={statistics.mean(vals):.3f}x"
    )
if rows and any(r.get("profile_enabled") == "1" for r in rows):
    task_rows = [r for r in rows if int(r.get("total_tasks") or 0) > 0]
    if task_rows:
        print(
            "profile: "
            f"tasks_avg={statistics.mean(int(r['total_tasks']) for r in task_rows):.1f} "
            f"dag_edges_avg={statistics.mean(int(r['dag_edges']) for r in task_rows):.1f} "
            f"queue_ms_avg={statistics.mean(float(r['queue_ms']) for r in task_rows):.3f} "
            f"exec_ms_avg={statistics.mean(float(r['exec_ms']) for r in task_rows):.3f}"
        )
PY
