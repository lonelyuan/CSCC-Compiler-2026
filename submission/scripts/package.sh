#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUBMISSION_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${SUBMISSION_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/submission_bisheng}"
DIST_DIR="${DIST_DIR:-${REPO_ROOT}/dist/submission}"

"${SUBMISSION_DIR}/scripts/build.sh"

rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}/pass" "${DIST_DIR}/runtime" "${DIST_DIR}/docs" "${DIST_DIR}/src"

cp "${BUILD_DIR}/pass/libcontestant_pass.so" "${DIST_DIR}/pass/"
cp "${BUILD_DIR}/runtime/libcontestant_runtime.a" "${DIST_DIR}/runtime/"
cp "${SUBMISSION_DIR}/manifest.json" "${DIST_DIR}/manifest.json"
cp "${SUBMISSION_DIR}/README.md" "${DIST_DIR}/README.md"
cp "${SUBMISSION_DIR}/docs/design.md" "${DIST_DIR}/docs/design.md"

tar -C "$(dirname "${DIST_DIR}")" -czf "${REPO_ROOT}/dist/submission.tar.gz" "$(basename "${DIST_DIR}")"

echo "Packaged:"
echo "  ${DIST_DIR}"
echo "  ${REPO_ROOT}/dist/submission.tar.gz"

