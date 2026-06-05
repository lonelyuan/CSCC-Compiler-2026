#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/case_parallel_experiment"
SPEC_START="${SPEC_START:-43}"
SPEC_END="${SPEC_END:-56}"
GENERATOR_THREADS="${GENERATOR_THREADS:-4}"
VERIFIER_THREADS="${VERIFIER_THREADS:-4}"
CASE_THREADS="${CASE_THREADS:-4}"
CXX="${CXX:-clang++}"

mkdir -p "${BUILD_DIR}/bin"

cd "${ROOT_DIR}"

"${CXX}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_generator/spd_generator.cpp \
  -o "${BUILD_DIR}/bin/spd_generator"

"${CXX}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_verifier/verifier.cpp \
  -o "${BUILD_DIR}/bin/verifier"

"${CXX}" -std=c++17 -O2 -pthread \
  -Iinclude -Isrc/base_kernels \
  src/base_kernels/kernels_public.cpp \
  src/base_kernels/kernels_impl.cpp \
  src/baseline/main.cpp \
  src/baseline/block_cholesky.cpp \
  -o "${BUILD_DIR}/bin/baseline_serial"

"${CXX}" -std=c++17 -O2 -pthread \
  -Iinclude -Isrc/base_kernels \
  src/base_kernels/kernels_public.cpp \
  src/base_kernels/kernels_impl.cpp \
  experiments/case_parallel_main.cpp \
  src/baseline/block_cholesky.cpp \
  -o "${BUILD_DIR}/bin/case_parallel"

sed -n "${SPEC_START},${SPEC_END}p" \
  cases/preliminary_public_150.txt > "${BUILD_DIR}/bench.spec"

echo "Spec lines: ${SPEC_START}-${SPEC_END}"
cat "${BUILD_DIR}/bench.spec"

SPD_GENERATOR_THREADS="${GENERATOR_THREADS}" \
  "${BUILD_DIR}/bin/spd_generator" \
  "${BUILD_DIR}/bench.bin" \
  --spec "${BUILD_DIR}/bench.spec"

COMPILER2026_TIMING_FILE="${BUILD_DIR}/serial.time" \
  "${BUILD_DIR}/bin/baseline_serial" \
  "${BUILD_DIR}/bench.bin" \
  "${BUILD_DIR}/serial.out"

VERIFIER_THREADS="${VERIFIER_THREADS}" \
  "${BUILD_DIR}/bin/verifier" \
  "${BUILD_DIR}/bench.bin" \
  "${BUILD_DIR}/serial.out" > "${BUILD_DIR}/serial.verify"

COMPILER2026_TIMING_FILE="${BUILD_DIR}/case_parallel.time" \
COMPILER2026_CASE_THREADS="${CASE_THREADS}" \
  "${BUILD_DIR}/bin/case_parallel" \
  "${BUILD_DIR}/bench.bin" \
  "${BUILD_DIR}/case_parallel.out"

VERIFIER_THREADS="${VERIFIER_THREADS}" \
  "${BUILD_DIR}/bin/verifier" \
  "${BUILD_DIR}/bench.bin" \
  "${BUILD_DIR}/case_parallel.out" > "${BUILD_DIR}/case_parallel.verify"

SERIAL_TIME="$(cat "${BUILD_DIR}/serial.time")"
PARALLEL_TIME="$(cat "${BUILD_DIR}/case_parallel.time")"

python3 - "${SERIAL_TIME}" "${PARALLEL_TIME}" <<'PY'
import sys
serial = float(sys.argv[1])
parallel = float(sys.argv[2])
speedup = serial / parallel if parallel > 0 else float("inf")
print(f"serial_seconds={serial:.9f}")
print(f"case_parallel_seconds={parallel:.9f}")
print(f"speedup={speedup:.3f}x")
PY

echo "serial_verify=${BUILD_DIR}/serial.verify"
echo "case_parallel_verify=${BUILD_DIR}/case_parallel.verify"
