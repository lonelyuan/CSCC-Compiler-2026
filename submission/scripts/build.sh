#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUBMISSION_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${SUBMISSION_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/submission_bisheng}"
LLVM_CONFIG="${LLVM_CONFIG:-/opt/bisheng/bin/llvm-config}"
CC_BIN="${CC:-/opt/bisheng/bin/clang}"
CXX_BIN="${CXX:-/opt/bisheng/bin/clang++}"

cmake -S "${SUBMISSION_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DLLVM_CONFIG="${LLVM_CONFIG}" \
  -DCMAKE_C_COMPILER="${CC_BIN}" \
  -DCMAKE_CXX_COMPILER="${CXX_BIN}"

cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "Built submission artifacts:"
echo "  ${BUILD_DIR}/pass/libcontestant_pass.so"
echo "  ${BUILD_DIR}/runtime/libcontestant_runtime.a"
echo "  ${BUILD_DIR}/manifest.json"

