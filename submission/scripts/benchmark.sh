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
echo "label,suite,repeat,threads,serial_seconds,contestant_seconds,speedup" > "${CSV}"

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

    COMPILER2026_TIMING_FILE="${suite_dir}/contestant_${run}.time" \
    COMPILER2026_DAG_THREADS="${THREADS}" \
      "${BENCH_DIR}/bin/contestant_app" \
      "${suite_dir}/input.bin" \
      "${suite_dir}/contestant_${run}.out"

    if [[ "${run}" == "1" ]]; then
      VERIFIER_THREADS="${VERIFIER_THREADS:-4}" \
        "${BENCH_DIR}/bin/verifier" \
        "${suite_dir}/input.bin" \
        "${suite_dir}/contestant_${run}.out" > "${suite_dir}/contestant.verify"
    fi

    python3 - "${LABEL}" "${suite}" "${run}" "${THREADS}" \
      "${suite_dir}/serial_${run}.time" \
      "${suite_dir}/contestant_${run}.time" >> "${CSV}" <<'PY'
import sys
label, suite, run, threads, serial_path, contestant_path = sys.argv[1:]
serial = float(open(serial_path).read())
contestant = float(open(contestant_path).read())
speedup = serial / contestant if contestant > 0 else float("inf")
print(f"{label},{suite},{run},{threads},{serial:.9f},{contestant:.9f},{speedup:.6f}")
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
PY

