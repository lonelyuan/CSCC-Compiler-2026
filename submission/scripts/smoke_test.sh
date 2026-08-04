#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUBMISSION_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${SUBMISSION_DIR}/.." && pwd)"
SDK_DIR="${REPO_ROOT}/contestant_sdk"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/submission_bisheng}"
SMOKE_DIR="${REPO_ROOT}/build/smoke_submission"
CXX_BIN="${CXX:-/opt/bisheng/bin/clang++}"
CLANG_BIN="${CLANG:-/opt/bisheng/bin/clang++}"
OPT_BIN="${OPT:-/opt/bisheng/bin/opt}"
LLVM_LINK_BIN="${LLVM_LINK:-/opt/bisheng/bin/llvm-link}"
THREADS="${COMPILER2026_DAG_THREADS:-4}"
PROFILE="${COMPILER2026_DAG_PROFILE:-0}"
TASK_BATCH="${COMPILER2026_TASK_BATCH:-auto}"
ASYNC_MIN_B="${COMPILER2026_ASYNC_MIN_B:-12}"
ASYNC_MIN_BLOCKS="${COMPILER2026_ASYNC_MIN_BLOCKS:-2}"
DAG_MAX_LIVE="${COMPILER2026_DAG_MAX_LIVE:-0}"
DAG_PIN_WORKERS="${COMPILER2026_DAG_PIN_WORKERS:-0}"
DAG_CRITICAL_PRIORITY="${COMPILER2026_DAG_CRITICAL_PRIORITY:-0}"
ANNOTATED_BLOCK_CHOLESKY="${SUBMISSION_DIR}/src/baseline/block_cholesky.cpp"
ANNOTATED_MAIN="${SUBMISSION_DIR}/src/baseline/main.cpp"

"${SUBMISSION_DIR}/scripts/build.sh"

mkdir -p "${SMOKE_DIR}/bin" "${SMOKE_DIR}/ir"
cd "${SDK_DIR}"

"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_generator/spd_generator.cpp \
  -o "${SMOKE_DIR}/bin/spd_generator"

"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_verifier/verifier.cpp \
  -o "${SMOKE_DIR}/bin/verifier"

"${CXX_BIN}" -std=c++17 -O2 -pthread \
  -Iinclude -Isrc/base_kernels \
  src/base_kernels/kernels_public.cpp \
  src/base_kernels/kernels_impl.cpp \
  src/baseline/main.cpp \
  src/baseline/block_cholesky.cpp \
  -o "${SMOKE_DIR}/bin/baseline_serial"

"${CLANG_BIN}" -std=c++17 -O2 -Iinclude -Isrc/base_kernels \
  -emit-llvm -c "${ANNOTATED_MAIN}" \
  -o "${SMOKE_DIR}/ir/main.bc"

"${CLANG_BIN}" -std=c++17 -O2 -Iinclude -Isrc/base_kernels \
  -emit-llvm -c "${ANNOTATED_BLOCK_CHOLESKY}" \
  -o "${SMOKE_DIR}/ir/block_cholesky.bc"

"${LLVM_LINK_BIN}" \
  "${SMOKE_DIR}/ir/main.bc" \
  "${SMOKE_DIR}/ir/block_cholesky.bc" \
  -o "${SMOKE_DIR}/ir/app.bc"

"${OPT_BIN}" \
  -load-pass-plugin "${BUILD_DIR}/pass/libcontestant_pass.so" \
  -passes=contestant-pass \
  "${SMOKE_DIR}/ir/app.bc" \
  -o "${SMOKE_DIR}/ir/app.opt.bc"

"${CXX_BIN}" -std=c++17 -O2 -pthread \
  -Iinclude -Isrc/base_kernels \
  "${SMOKE_DIR}/ir/app.opt.bc" \
  src/base_kernels/kernels_public.cpp \
  src/base_kernels/kernels_impl.cpp \
  "${BUILD_DIR}/runtime/libcontestant_runtime.a" \
  -o "${SMOKE_DIR}/bin/contestant_app"

sed -n "${SPEC_START:-43},${SPEC_END:-56}p" \
  cases/preliminary_public_150.txt > "${SMOKE_DIR}/smoke.spec"

SPD_GENERATOR_THREADS="${GENERATOR_THREADS:-4}" \
  "${SMOKE_DIR}/bin/spd_generator" \
  "${SMOKE_DIR}/input.bin" \
  --spec "${SMOKE_DIR}/smoke.spec"

COMPILER2026_TIMING_FILE="${SMOKE_DIR}/serial.time" \
  "${SMOKE_DIR}/bin/baseline_serial" \
  "${SMOKE_DIR}/input.bin" \
  "${SMOKE_DIR}/serial.out"

COMPILER2026_TIMING_FILE="${SMOKE_DIR}/contestant.time" \
COMPILER2026_DAG_THREADS="${THREADS}" \
COMPILER2026_DAG_PROFILE="${PROFILE}" \
COMPILER2026_TASK_BATCH="${TASK_BATCH}" \
COMPILER2026_ASYNC_MIN_B="${ASYNC_MIN_B}" \
COMPILER2026_ASYNC_MIN_BLOCKS="${ASYNC_MIN_BLOCKS}" \
COMPILER2026_DAG_MAX_LIVE="${DAG_MAX_LIVE}" \
COMPILER2026_DAG_PIN_WORKERS="${DAG_PIN_WORKERS}" \
COMPILER2026_DAG_CRITICAL_PRIORITY="${DAG_CRITICAL_PRIORITY}" \
  "${SMOKE_DIR}/bin/contestant_app" \
  "${SMOKE_DIR}/input.bin" \
  "${SMOKE_DIR}/contestant.out"

VERIFIER_THREADS="${VERIFIER_THREADS:-4}" \
  "${SMOKE_DIR}/bin/verifier" \
  "${SMOKE_DIR}/input.bin" \
  "${SMOKE_DIR}/serial.out" > "${SMOKE_DIR}/serial.verify"

VERIFIER_THREADS="${VERIFIER_THREADS:-4}" \
  "${SMOKE_DIR}/bin/verifier" \
  "${SMOKE_DIR}/input.bin" \
  "${SMOKE_DIR}/contestant.out" > "${SMOKE_DIR}/contestant.verify"

python3 - "${SMOKE_DIR}/serial.time" "${SMOKE_DIR}/contestant.time" <<'PY'
import sys
serial = float(open(sys.argv[1]).read())
contestant = float(open(sys.argv[2]).read())
speedup = serial / contestant if contestant > 0 else float("inf")
print(f"serial_seconds={serial:.9f}")
print(f"contestant_seconds={contestant:.9f}")
print(f"speedup={speedup:.3f}x")
PY

echo "contestant_app=${SMOKE_DIR}/bin/contestant_app"
echo "serial_verify=${SMOKE_DIR}/serial.verify"
echo "contestant_verify=${SMOKE_DIR}/contestant.verify"
