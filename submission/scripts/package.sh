#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUBMISSION_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${SUBMISSION_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/submission_bisheng}"
DIST_DIR="${DIST_DIR:-${REPO_ROOT}/dist/submission}"

"${SUBMISSION_DIR}/scripts/build.sh"

rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}/pass" "${DIST_DIR}/runtime" "${DIST_DIR}/docs" "${DIST_DIR}/scripts"

cp "${SUBMISSION_DIR}/CMakeLists.txt" "${DIST_DIR}/CMakeLists.txt"
cp "${SUBMISSION_DIR}/manifest.json" "${DIST_DIR}/manifest.json"
cp "${SUBMISSION_DIR}/README.md" "${DIST_DIR}/README.md"
cp "${SUBMISSION_DIR}/docs/design.md" "${DIST_DIR}/docs/design.md"
cp "${SUBMISSION_DIR}/docs/performance.md" "${DIST_DIR}/docs/performance.md"
cp "${SUBMISSION_DIR}/docs/roadmap.md" "${DIST_DIR}/docs/roadmap.md"
cp -R "${SUBMISSION_DIR}/docs/benchmark_results" "${DIST_DIR}/docs/benchmark_results"
cp -R "${SUBMISSION_DIR}/pass/." "${DIST_DIR}/pass/"
cp -R "${SUBMISSION_DIR}/runtime/." "${DIST_DIR}/runtime/"
cp -R "${SUBMISSION_DIR}/scripts/." "${DIST_DIR}/scripts/"
cp "${BUILD_DIR}/pass/libcontestant_pass.so" "${DIST_DIR}/pass/"
cp "${BUILD_DIR}/runtime/libcontestant_runtime.a" "${DIST_DIR}/runtime/"

tar -C "${DIST_DIR}" -czf "${REPO_ROOT}/dist/submission.tar.gz" .
python3 - "${DIST_DIR}" "${REPO_ROOT}/dist/submission.zip" <<'PY'
import os
import sys
import zipfile

dist_dir, zip_path = sys.argv[1:]

with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
    for current, _, files in os.walk(dist_dir):
        for name in files:
            path = os.path.join(current, name)
            rel = os.path.relpath(path, dist_dir)
            archive.write(path, rel)
PY

echo "Packaged:"
echo "  ${DIST_DIR}"
echo "  ${REPO_ROOT}/dist/submission.tar.gz"
echo "  ${REPO_ROOT}/dist/submission.zip"
