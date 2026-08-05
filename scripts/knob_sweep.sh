#!/usr/bin/env bash
# Sweep ONE runtime environment knob across values, per case, with the verifier
# run at every point.
#
# This generalises scripts/participant_sweep.sh (which sweeps only
# COMPILER2026_DAG_THREADS). Round 14 swept COMPILER2026_RANGE_TASK_FLOPS by
# hand; this script is that experiment made repeatable, which matters because
# every tuned constant in the runtime carries a "PLATFORM CAVEAT: re-measure on
# aarch64" comment and re-deriving them needs one command per knob.
#
# The reported number for a point is the per-case MINIMUM over REPEAT in-process
# repetitions, which suppresses turbo/thermal noise but does NOT model the judge's
# first-call cost -- use scripts/percase_bench_chunked.sh to validate a chosen
# value over the full suite.
#
# Usage (on the benchmark host, with the toolchain env loaded):
#   KNOB=COMPILER2026_RANGE_TASK_FLOPS VALUES="12500 25000 50000 100000 200000" \
#     CASES="1152:8 1152:16 1152:32 1152:128" ./scripts/knob_sweep.sh
#
#   # a second knob can be pinned for the whole sweep:
#   KNOB=COMPILER2026_DAG_THREADS VALUES="8 16 24 32 40" \
#     PIN="COMPILER2026_DAG_PARTICIPANT_CAP=off" ./scripts/knob_sweep.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SUBMISSION_DIR="${REPO_ROOT}/submission"
SDK_DIR="${REPO_ROOT}/contestant_sdk"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/submission_bisheng}"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/build/knob_sweep}"

CXX_BIN="${CXX:-/opt/bisheng/bin/clang++}"
CLANG_BIN="${CLANG:-/opt/bisheng/bin/clang++}"
OPT_BIN="${OPT:-/opt/bisheng/bin/opt}"

KNOB="${KNOB:?KNOB required, e.g. KNOB=COMPILER2026_RANGE_TASK_FLOPS}"
VALUES="${VALUES:?VALUES required, e.g. VALUES=\"25000 50000 100000\"}"
CASES="${CASES:-1152:8 1152:16 1152:32 1152:64 1152:128}"
REPEAT="${REPEAT:-3}"
# Extra env assignments held constant for every point, space separated K=V.
PIN="${PIN:-}"
CSV_OUT="${CSV_OUT:-}"
TASKSET_CPUS="${TASKSET_CPUS:-0-39}"
if [[ -n "${TASKSET_CPUS}" ]] && command -v taskset >/dev/null 2>&1; then
  RUNNER=(taskset -c "${TASKSET_CPUS}")
else
  RUNNER=()
fi

HARNESS_MAIN="${REPO_ROOT}/tools/percase_harness/main_percase.cpp"
BLOCK_CHOLESKY="${SUBMISSION_DIR}/src/baseline/block_cholesky.cpp"

"${SUBMISSION_DIR}/scripts/build.sh" >/dev/null
mkdir -p "${WORK_DIR}/bin" "${WORK_DIR}/ir"
cd "${SDK_DIR}"

COMMON_CXX=(-std=c++17 -O2 -pthread -Iinclude -Isrc/base_kernels)
KERNELS=(src/base_kernels/kernels_public.cpp src/base_kernels/kernels_impl.cpp)

echo "[1/3] building"
"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_generator/spd_generator.cpp -o "${WORK_DIR}/bin/spd_generator"
"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_verifier/verifier.cpp -o "${WORK_DIR}/bin/verifier"
"${CXX_BIN}" "${COMMON_CXX[@]}" "${KERNELS[@]}" "${HARNESS_MAIN}" \
  "${BLOCK_CHOLESKY}" -o "${WORK_DIR}/bin/serial"
"${CLANG_BIN}" -std=c++17 -O2 -Iinclude -Isrc/base_kernels \
  -emit-llvm -c "${BLOCK_CHOLESKY}" -o "${WORK_DIR}/ir/bc.bc"
"${OPT_BIN}" -load-pass-plugin "${BUILD_DIR}/pass/libcontestant_pass.so" \
  -passes=contestant-pass "${WORK_DIR}/ir/bc.bc" -o "${WORK_DIR}/ir/bc.opt.bc"
"${CXX_BIN}" "${COMMON_CXX[@]}" "${WORK_DIR}/ir/bc.opt.bc" "${KERNELS[@]}" \
  "${HARNESS_MAIN}" "${BUILD_DIR}/runtime/libcontestant_runtime.a" \
  -o "${WORK_DIR}/bin/contestant"

read -r -a PIN_ARGS <<< "${PIN}"

echo "[2/3] sweeping ${KNOB} (repeat=${REPEAT}${PIN:+, pinned: ${PIN}})"
printf "  %-14s %-11s" "case" "serial_s"
for value in ${VALUES}; do printf "%11s" "${value}"; done
printf "\n"

if [[ -n "${CSV_OUT}" ]]; then
  echo "case_n,case_b,knob,value,serial_seconds,contestant_seconds,speedup" > "${CSV_OUT}"
fi

for case_spec in ${CASES}; do
  n="${case_spec%%:*}"
  b="${case_spec##*:}"
  case_dir="${WORK_DIR}/${n}_${b}"
  mkdir -p "${case_dir}"
  printf "%s:%s:1\n" "${n}" "${b}" > "${case_dir}/spec.txt"
  if [[ ! -f "${case_dir}/input.bin" ]]; then
    SPD_GENERATOR_THREADS="${GENERATOR_THREADS:-40}" \
      "${WORK_DIR}/bin/spd_generator" "${case_dir}/input.bin" \
      --spec "${case_dir}/spec.txt" >/dev/null
  fi

  COMPILER2026_PERCASE_CSV="${case_dir}/serial.csv" \
  COMPILER2026_PERCASE_REPEAT="${REPEAT}" \
    "${RUNNER[@]}" "${WORK_DIR}/bin/serial" \
    "${case_dir}/input.bin" "${case_dir}/serial.out" >/dev/null
  serial=$(awk -F, 'NR==2 {print $5}' "${case_dir}/serial.csv")

  printf "  %-14s %-11.6f" "n=${n} b=${b}" "${serial}"
  for value in ${VALUES}; do
    COMPILER2026_PERCASE_CSV="${case_dir}/c_${value}.csv" \
    COMPILER2026_PERCASE_REPEAT="${REPEAT}" \
      env "${KNOB}=${value}" ${PIN_ARGS[@]+"${PIN_ARGS[@]}"} \
      "${RUNNER[@]}" "${WORK_DIR}/bin/contestant" \
      "${case_dir}/input.bin" "${case_dir}/out_${value}.bin" >/dev/null
    contestant=$(awk -F, 'NR==2 {print $5}' "${case_dir}/c_${value}.csv")
    # A fast wrong answer is not a result: every reported point is verified.
    VERIFIER_THREADS="${VERIFIER_THREADS:-40}" \
      "${WORK_DIR}/bin/verifier" "${case_dir}/input.bin" "${case_dir}/out_${value}.bin" \
      > "${case_dir}/v_${value}.txt"
    if ! grep -q "status=PASS" "${case_dir}/v_${value}.txt"; then
      printf "%11s" "FAIL"
      continue
    fi
    speedup=$(awk -v s="${serial}" -v c="${contestant}" 'BEGIN{printf "%.4f", s/c}')
    printf "%11s" "$(awk -v v="${speedup}" 'BEGIN{printf "%.2fx", v}')"
    if [[ -n "${CSV_OUT}" ]]; then
      echo "${n},${b},${KNOB},${value},${serial},${contestant},${speedup}" >> "${CSV_OUT}"
    fi
    rm -f "${case_dir}/out_${value}.bin"
  done
  printf "\n"
done

echo "[3/3] done (every reported point passed the verifier)"
if [[ -n "${CSV_OUT}" ]]; then
  echo "csv=${CSV_OUT}"
fi
