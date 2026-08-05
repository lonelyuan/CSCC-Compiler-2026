#!/usr/bin/env bash
# Measure one case across participant counts with the participant cap disabled.
#
# This is the experiment that exposed the runtime's scalability defect: Round 6
# measured n=1152 b=16 peaking at 8 participants and falling to 1.15x by 40, while
# a hand-written probe on the same case rose monotonically to 8.99x. Re-run it
# after any scheduling change to see whether the curve still turns over.
#
# The cap is forced off so the curve is the runtime's own, not the cap's. Use it to
# re-derive participantCapForTile rather than to pick a default directly.
#
# Usage (on the benchmark host, with the toolchain env loaded):
#   source ~/llvm17.env && CASES="1152:16 1152:8" ./scripts/participant_sweep.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SUBMISSION_DIR="${REPO_ROOT}/submission"
SDK_DIR="${REPO_ROOT}/contestant_sdk"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/submission_bisheng}"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/build/participant_sweep}"

CXX_BIN="${CXX:-/opt/bisheng/bin/clang++}"
CLANG_BIN="${CLANG:-/opt/bisheng/bin/clang++}"
OPT_BIN="${OPT:-/opt/bisheng/bin/opt}"

CASES="${CASES:-1152:16 1152:12 1152:8 2048:8 1792:32}"
THREAD_LIST="${THREAD_LIST:-4 8 16 24 32 40}"
REPEAT="${REPEAT:-3}"
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

echo "[2/3] measuring (cap off, repeat=${REPEAT})"
printf "  %-12s %-10s" "case" "serial_s"
for t in ${THREAD_LIST}; do printf "%9s" "T=${t}"; done
printf "\n"

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

  printf "  %-12s %-10.6f" "n=${n} b=${b}" "${serial}"
  for t in ${THREAD_LIST}; do
    COMPILER2026_PERCASE_CSV="${case_dir}/c_${t}.csv" \
    COMPILER2026_PERCASE_REPEAT="${REPEAT}" \
      env COMPILER2026_DAG_PARTICIPANT_CAP=off COMPILER2026_DAG_THREADS="${t}" \
      "${RUNNER[@]}" "${WORK_DIR}/bin/contestant" \
      "${case_dir}/input.bin" "${case_dir}/out_${t}.bin" >/dev/null
    contestant=$(awk -F, 'NR==2 {print $5}' "${case_dir}/c_${t}.csv")
    # Correctness is checked for every point: a fast wrong answer is not a result.
    VERIFIER_THREADS="${VERIFIER_THREADS:-40}" \
      "${WORK_DIR}/bin/verifier" "${case_dir}/input.bin" "${case_dir}/out_${t}.bin" \
      > "${case_dir}/v_${t}.txt"
    if ! grep -q "status=PASS" "${case_dir}/v_${t}.txt"; then
      printf "%9s" "FAIL"
      continue
    fi
    printf "%9s" "$(awk -v s="${serial}" -v c="${contestant}" 'BEGIN{printf "%.2fx", s/c}')"
  done
  printf "\n"
done

echo "[3/3] done (every reported point passed the verifier)"
