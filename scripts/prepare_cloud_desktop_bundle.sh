#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: prepare_cloud_desktop_bundle.sh [label]

Create a source bundle for the CourseGrading cloud desktop. The bundle contains
the submission sources, the official SDK sources, and a guarded smoke-test
runner. It never uploads files or contacts the cloud desktop.

Environment:
  CLOUD_DESKTOP_OUT_DIR  Output directory (default: build/cloud_desktop)
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

LABEL="${1:-cloud_smoke_$(date -u +%Y%m%dT%H%M%SZ)}"
if [[ ! "${LABEL}" =~ ^[A-Za-z0-9._-]+$ ]]; then
  printf 'invalid label (use only A-Z, a-z, 0-9, ., _, -): %s\n' "${LABEL}" >&2
  exit 64
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_ROOT="${CLOUD_DESKTOP_OUT_DIR:-${REPO_ROOT}/build/cloud_desktop}"
RUN_NAME="cloud_eval_${LABEL}"
BUNDLE_NAME="${RUN_NAME}.zip"
RUN_ROOT="${OUTPUT_ROOT}/${RUN_NAME}"
BUNDLE_PATH="${OUTPUT_ROOT}/${BUNDLE_NAME}"
COMMAND_PATH="${RUN_ROOT}/remote-command.txt"
STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/bisheng-cloud-desktop.XXXXXX")"

cleanup() {
  rm -rf "${STAGE_DIR}"
}
trap cleanup EXIT

mkdir -p "${OUTPUT_ROOT}" "${RUN_ROOT}"
rm -f "${BUNDLE_PATH}" "${COMMAND_PATH}"
mkdir -p "${STAGE_DIR}/project"

cp -R "${REPO_ROOT}/submission" "${STAGE_DIR}/project/submission"
cp -R "${REPO_ROOT}/contestant_sdk" "${STAGE_DIR}/project/contestant_sdk"
rm -rf "${STAGE_DIR}/project/contestant_sdk/bin" "${STAGE_DIR}/project/contestant_sdk/lib"
find "${STAGE_DIR}" -name .DS_Store -type f -delete
cp "${SCRIPT_DIR}/cloud_desktop_run.sh" "${STAGE_DIR}/run.sh"
chmod +x "${STAGE_DIR}/run.sh"

GIT_REVISION="$(git -C "${REPO_ROOT}" rev-parse --verify HEAD 2>/dev/null || printf 'uncommitted')"
{
  printf 'label=%s\n' "${LABEL}"
  printf 'prepared_at_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'git_revision=%s\n' "${GIT_REVISION}"
  printf 'default_test_command=%s\n' \
    'SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh'
} > "${STAGE_DIR}/manifest.env"

(
  cd "${STAGE_DIR}"
  zip -qr "${BUNDLE_PATH}" project run.sh manifest.env
)

BUNDLE_SIZE="$(wc -c < "${BUNDLE_PATH}" | tr -d '[:space:]')"
MAX_SIZE=$((64 * 1024 * 1024))
if (( BUNDLE_SIZE > MAX_SIZE )); then
  printf 'bundle exceeds cloud desktop limit: %s bytes > %s bytes\n' \
    "${BUNDLE_SIZE}" "${MAX_SIZE}" >&2
  exit 65
fi

if command -v shasum >/dev/null 2>&1; then
  BUNDLE_SHA256="$(shasum -a 256 "${BUNDLE_PATH}" | awk '{print $1}')"
else
  BUNDLE_SHA256="$(sha256sum "${BUNDLE_PATH}" | awk '{print $1}')"
fi

{
  printf 'cd /mnt/cgshare && '
  printf 'rm -rf %q && ' "${RUN_NAME}"
  printf 'mkdir -p %q && ' "${RUN_NAME}"
  printf 'python3 -m zipfile -e %q %q && ' "${BUNDLE_NAME}" "${RUN_NAME}"
  printf 'bash %q\n' "${RUN_NAME}/run.sh"
} > "${COMMAND_PATH}"

printf 'bundle=%s\n' "${BUNDLE_PATH}"
printf 'bundle_size_bytes=%s\n' "${BUNDLE_SIZE}"
printf 'bundle_sha256=%s\n' "${BUNDLE_SHA256}"
printf 'remote_command=%s\n' "${COMMAND_PATH}"
printf 'result_file=/mnt/cgshare/%s/result.env\n' "${RUN_NAME}"
printf 'log_file=/mnt/cgshare/%s/test.log\n' "${RUN_NAME}"
