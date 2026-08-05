#!/usr/bin/env bash
# Per-case benchmark: measures every case separately and scores it the way the
# judge does (equal-weight geometric mean of per-case speedups).
#
# This is the measurement counterpart to `submission/scripts/benchmark.sh`, which
# reports a per-suite TOTAL-TIME ratio (flops-weighted, dominated by the largest
# cases) and therefore cannot see the metric the judge actually scores.
#
# Unlike benchmark.sh, this script FAILS if any verifier case reports something
# other than PASS -- a performance number without correctness evidence is not a
# result.
#
# Usage (on the benchmark host, with the toolchain env loaded):
#   LABEL=r9_percase_baseline REPEAT=3 ./scripts/percase_bench.sh
#   SPEC_START=1 SPEC_END=20 LABEL=smoke ./scripts/percase_bench.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SUBMISSION_DIR="${REPO_ROOT}/submission"
SDK_DIR="${REPO_ROOT}/contestant_sdk"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/submission_bisheng}"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/build/percase}"
RESULTS_DIR="${RESULTS_DIR:-${REPO_ROOT}/docs/benchmark_results}"

CXX_BIN="${CXX:-/opt/bisheng/bin/clang++}"
CLANG_BIN="${CLANG:-/opt/bisheng/bin/clang++}"
OPT_BIN="${OPT:-/opt/bisheng/bin/opt}"
LLVM_DIS_BIN="${LLVM_DIS:-/opt/bisheng/bin/llvm-dis}"

LABEL="${LABEL:-percase}"
# PASSES: whole-process repetitions. REPEAT: in-process repetitions of the same
# case. These are NOT interchangeable. The judge runs one process over all 150
# cases, so any per-case setup the runtime does (it rebuilds the worker pool
# whenever the resolved thread count changes between cases) is paid on the FIRST
# call for that case. An in-process repeat reuses the pool and hides that cost,
# so the faithful configuration is REPEAT=1 with PASSES>1, taking the per-case
# minimum across passes to suppress turbo/thermal noise.
PASSES="${PASSES:-3}"
REPEAT="${REPEAT:-1}"
SPEC_START="${SPEC_START:-1}"
SPEC_END="${SPEC_END:-150}"
M_IDEAL="${M_IDEAL:-32.0}"
STAT="${STAT:-min}"
# Bind to physical cores by default so results stay comparable with the
# Round 1-8 numbers in docs/engineering_log.md (this host has 40 physical /
# 80 logical cores; SMT siblings only add noise).
TASKSET_CPUS="${TASKSET_CPUS:-0-39}"
if [[ -n "${TASKSET_CPUS}" ]] && command -v taskset >/dev/null 2>&1; then
  RUNNER=(taskset -c "${TASKSET_CPUS}")
else
  RUNNER=()
fi

ANNOTATED_BLOCK_CHOLESKY="${SUBMISSION_DIR}/src/baseline/block_cholesky.cpp"
HARNESS_MAIN="${REPO_ROOT}/tools/percase_harness/main_percase.cpp"

"${SUBMISSION_DIR}/scripts/build.sh" >/dev/null

mkdir -p "${WORK_DIR}/bin" "${WORK_DIR}/ir" "${WORK_DIR}/${LABEL}" "${RESULTS_DIR}"
CASE_DIR="${WORK_DIR}/${LABEL}"
cd "${SDK_DIR}"

echo "[1/6] building tools"
"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_generator/spd_generator.cpp -o "${WORK_DIR}/bin/spd_generator"
"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_verifier/verifier.cpp -o "${WORK_DIR}/bin/verifier"

# Serial reference: official SDK block_cholesky, official operators, per-case
# timing harness instead of the accumulating official main.
"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude -Isrc/base_kernels \
  src/base_kernels/kernels_public.cpp \
  src/base_kernels/kernels_impl.cpp \
  "${HARNESS_MAIN}" \
  src/baseline/block_cholesky.cpp \
  -o "${WORK_DIR}/bin/baseline_percase"

echo "[2/6] running pass on block_cholesky"
# The pass only needs `contest::block_cholesky`; main.cpp carries no annotation
# (it is byte-identical to the official one), so it is left out of the module and
# the harness main is linked in as a normal translation unit.
"${CLANG_BIN}" -std=c++17 -O2 -Iinclude -Isrc/base_kernels \
  -emit-llvm -c "${ANNOTATED_BLOCK_CHOLESKY}" \
  -o "${WORK_DIR}/ir/block_cholesky.bc"

"${OPT_BIN}" -load-pass-plugin "${BUILD_DIR}/pass/libcontestant_pass.so" \
  -passes=contestant-pass \
  "${WORK_DIR}/ir/block_cholesky.bc" \
  -o "${WORK_DIR}/ir/block_cholesky.opt.bc"

"${LLVM_DIS_BIN}" "${WORK_DIR}/ir/block_cholesky.opt.bc" -o "${WORK_DIR}/ir/block_cholesky.opt.ll"
IR_LL="${WORK_DIR}/ir/block_cholesky.opt.ll"
# The default path submits without dependency keys (phase barrier between trsm and
# madd), while the opt-in cross-panel paths use submit_deps*. Count both and assert
# on the total, so this check keeps working either way.
IR_SUBMIT_PLAIN=$(grep -Ec "call void @compiler2026_runtime_submit\(" "${IR_LL}" || true)
IR_SUBMIT_DEPS=$(grep -Ec "call void @compiler2026_runtime_submit_deps3?(_priority)?\(" "${IR_LL}" || true)
IR_SUBMITS=$((IR_SUBMIT_PLAIN + IR_SUBMIT_DEPS))
IR_WAIT_CALLS=$(grep -Ec "call void @compiler2026_runtime_wait\(" "${IR_LL}" || true)
IR_TRSM_CALLS=$(grep -Ec "call .*@trsm\(" "${IR_LL}" || true)
IR_MADD_CALLS=$(grep -Ec "call .*@madd\(" "${IR_LL}" || true)
echo "      ir_submits=${IR_SUBMITS} (plain=${IR_SUBMIT_PLAIN} deps=${IR_SUBMIT_DEPS})" \
     "ir_wait_calls=${IR_WAIT_CALLS}" \
     "ir_trsm_calls=${IR_TRSM_CALLS} ir_madd_calls=${IR_MADD_CALLS}"
if [[ "${IR_SUBMITS}" -eq 0 || "${IR_TRSM_CALLS}" -eq 0 || "${IR_MADD_CALLS}" -eq 0 ]]; then
  echo "percase_bench: pass did not transform block_cholesky" >&2
  exit 3
fi
# Two barriers per panel are expected on the default path: one after the trsm loop
# and one at the end of the madd nest. Zero waits would mean madd can race trsm.
if [[ "${IR_WAIT_CALLS}" -eq 0 ]]; then
  echo "percase_bench: no runtime_wait in transformed IR -- madd could race trsm" >&2
  exit 3
fi

"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude -Isrc/base_kernels \
  "${WORK_DIR}/ir/block_cholesky.opt.bc" \
  src/base_kernels/kernels_public.cpp \
  src/base_kernels/kernels_impl.cpp \
  "${HARNESS_MAIN}" \
  "${BUILD_DIR}/runtime/libcontestant_runtime.a" \
  -o "${WORK_DIR}/bin/contestant_percase"

echo "[3/6] generating input (spec lines ${SPEC_START}-${SPEC_END})"
# INPUT_BIN lets a later run reuse the 1.2 GB input from an earlier one. The spec
# is still written and compared so a reused input cannot silently belong to a
# different case set.
sed -n "${SPEC_START},${SPEC_END}p" cases/preliminary_public_150.txt > "${CASE_DIR}/spec.txt"
if [[ -n "${INPUT_BIN:-}" ]]; then
  if [[ ! -f "${INPUT_BIN}" ]]; then
    echo "percase_bench: INPUT_BIN not found: ${INPUT_BIN}" >&2
    exit 5
  fi
  reused_spec="$(dirname "${INPUT_BIN}")/spec.txt"
  if [[ -f "${reused_spec}" ]] && ! cmp -s "${reused_spec}" "${CASE_DIR}/spec.txt"; then
    echo "percase_bench: INPUT_BIN was generated from a different spec" >&2
    exit 5
  fi
  echo "      reusing ${INPUT_BIN}"
  INPUT_PATH="${INPUT_BIN}"
else
  SPD_GENERATOR_THREADS="${GENERATOR_THREADS:-40}" \
    "${WORK_DIR}/bin/spd_generator" "${CASE_DIR}/input.bin" \
    --spec "${CASE_DIR}/spec.txt" >/dev/null
  INPUT_PATH="${CASE_DIR}/input.bin"
fi

echo "[4/6] serial reference (passes=${PASSES}, in-process repeat=${REPEAT})"
serial_csvs=()
for pass_index in $(seq 1 "${PASSES}"); do
  COMPILER2026_PERCASE_CSV="${CASE_DIR}/serial_percase_p${pass_index}.csv" \
  COMPILER2026_PERCASE_REPEAT="${REPEAT}" \
  COMPILER2026_TIMING_FILE="${CASE_DIR}/serial_p${pass_index}.time" \
    "${RUNNER[@]}" "${WORK_DIR}/bin/baseline_percase" \
    "${INPUT_PATH}" "${CASE_DIR}/serial.out" >/dev/null
  serial_csvs+=("${CASE_DIR}/serial_percase_p${pass_index}.csv")
done
python3 "${SCRIPT_DIR}/merge_percase.py" --output "${CASE_DIR}/serial_percase.csv" \
  "${serial_csvs[@]}"

echo "[5/6] contestant (passes=${PASSES}, in-process repeat=${REPEAT})"
env_args=()
for var in COMPILER2026_DAG_THREADS COMPILER2026_DAG_PROFILE COMPILER2026_TASK_BATCH \
           COMPILER2026_ASYNC_MIN_B COMPILER2026_ASYNC_MIN_BLOCKS COMPILER2026_DAG_MAX_LIVE \
           COMPILER2026_DAG_PIN_WORKERS COMPILER2026_DAG_CRITICAL_PRIORITY \
           COMPILER2026_DAG_PARTICIPANT_CAP COMPILER2026_MADD_SUBTILE; do
  if [[ -n "${!var:-}" ]]; then
    env_args+=("${var}=${!var}")
  fi
done
echo "      runtime env: ${env_args[*]:-<defaults>}"
contestant_csvs=()
for pass_index in $(seq 1 "${PASSES}"); do
  COMPILER2026_PERCASE_CSV="${CASE_DIR}/contestant_percase_p${pass_index}.csv" \
  COMPILER2026_PERCASE_REPEAT="${REPEAT}" \
  COMPILER2026_TIMING_FILE="${CASE_DIR}/contestant_p${pass_index}.time" \
    env "${env_args[@]}" "${RUNNER[@]}" "${WORK_DIR}/bin/contestant_percase" \
    "${INPUT_PATH}" "${CASE_DIR}/contestant.out" \
    > "${CASE_DIR}/contestant_p${pass_index}.stdout"
  contestant_csvs+=("${CASE_DIR}/contestant_percase_p${pass_index}.csv")
done
python3 "${SCRIPT_DIR}/merge_percase.py" --output "${CASE_DIR}/contestant_percase.csv" \
  "${contestant_csvs[@]}"
tail -1 "${CASE_DIR}/contestant_p${PASSES}.stdout"

echo "[6/6] verifying"
for side in serial contestant; do
  VERIFIER_THREADS="${VERIFIER_THREADS:-40}" \
    "${WORK_DIR}/bin/verifier" "${INPUT_PATH}" "${CASE_DIR}/${side}.out" \
    > "${CASE_DIR}/${side}.verify"
  pass=$(grep -c "status=PASS" "${CASE_DIR}/${side}.verify" || true)
  total=$(grep -c "^case=" "${CASE_DIR}/${side}.verify" || true)
  echo "      ${side}: ${pass}/${total} PASS"
  if [[ "${pass}" -ne "${total}" || "${total}" -eq 0 ]]; then
    echo "percase_bench: verifier failures in ${side}" >&2
    grep -v "status=PASS" "${CASE_DIR}/${side}.verify" | head -10 >&2
    exit 4
  fi
done

echo
python3 "${SCRIPT_DIR}/score_judge.py" \
  "${CASE_DIR}/serial_percase.csv" \
  "${CASE_DIR}/contestant_percase.csv" \
  --m-ideal "${M_IDEAL}" --stat "${STAT}" --label "${LABEL}" \
  --merged-csv "${RESULTS_DIR}/${LABEL}.csv"
