#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUBMISSION_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${SUBMISSION_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/submission_bisheng}"
LLVM_CONFIG="${LLVM_CONFIG:-/opt/bisheng/bin/llvm-config}"
CC_BIN="${CC:-/opt/bisheng/bin/clang}"
CXX_BIN="${CXX:-/opt/bisheng/bin/clang++}"
CMAKE_GENERATOR="${CMAKE_GENERATOR:-Ninja}"
if command -v nproc >/dev/null 2>&1; then
  DEFAULT_BUILD_JOBS="$(nproc)"
else
  DEFAULT_BUILD_JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
fi
BUILD_JOBS="${BUILD_JOBS:-${DEFAULT_BUILD_JOBS}}"

cmake -S "${SUBMISSION_DIR}" -B "${BUILD_DIR}" -G "${CMAKE_GENERATOR}" \
  -DLLVM_CONFIG="${LLVM_CONFIG}" \
  -DCMAKE_C_COMPILER="${CC_BIN}" \
  -DCMAKE_CXX_COMPILER="${CXX_BIN}"

cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS}"

echo "Built submission artifacts:"
echo "  ${BUILD_DIR}/pass/libcontestant_pass.so"
echo "  ${BUILD_DIR}/runtime/libcontestant_runtime.a"
echo "  ${BUILD_DIR}/manifest.json"
