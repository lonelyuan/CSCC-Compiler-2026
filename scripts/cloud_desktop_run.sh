#!/usr/bin/env bash
set -uo pipefail

RUN_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${RUN_ROOT}/project"
LOG_FILE="${RUN_ROOT}/test.log"
RESULT_FILE="${RUN_ROOT}/result.env"
TEST_COMMAND="${CLOUD_DESKTOP_TEST_COMMAND:-SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 ./submission/scripts/smoke_test.sh}"

write_result() {
  local status="$1"
  local exit_code="$2"
  {
    printf 'status=%q\n' "${status}"
    printf 'exit_code=%q\n' "${exit_code}"
    printf 'test_command=%q\n' "${TEST_COMMAND}"
    printf 'log_file=%q\n' "${LOG_FILE}"
    printf 'finished_at_utc=%q\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } > "${RESULT_FILE}"
}

if [[ ! -d "${PROJECT_DIR}" ]]; then
  printf 'missing project directory: %s\n' "${PROJECT_DIR}" > "${LOG_FILE}"
  write_result "setup_error" 64
  cat "${RESULT_FILE}"
  exit 64
fi

if [[ -r /etc/profile.d/bisheng.sh ]]; then
  # shellcheck disable=SC1091
  source /etc/profile.d/bisheng.sh
fi

MISSING_TOOLS=()
for required_tool in \
  cmake ninja \
  /opt/bisheng/bin/clang \
  /opt/bisheng/bin/clang++ \
  /opt/bisheng/bin/llvm-config \
  /opt/bisheng/bin/llvm-link \
  /opt/bisheng/bin/opt; do
  if ! command -v "${required_tool}" >/dev/null 2>&1; then
    MISSING_TOOLS+=("${required_tool}")
  fi
done
if (( ${#MISSING_TOOLS[@]} > 0 )); then
  {
    printf '%s\n' 'cloud desktop is missing required build tools:'
    printf '  %s\n' "${MISSING_TOOLS[@]}"
  } > "${LOG_FILE}"
  write_result "setup_error" 127
  cat "${RESULT_FILE}"
  printf '%s\n' '--- test.log (tail) ---'
  tail -n 60 "${LOG_FILE}" || true
  exit 127
fi

{
  printf 'run_root=%s\n' "${RUN_ROOT}"
  printf 'project_dir=%s\n' "${PROJECT_DIR}"
  printf 'test_command=%s\n' "${TEST_COMMAND}"
  cd "${PROJECT_DIR}"
  bash -lc "${TEST_COMMAND}"
} > "${LOG_FILE}" 2>&1
EXIT_CODE=$?

if [[ "${EXIT_CODE}" -eq 0 ]]; then
  write_result "passed" "${EXIT_CODE}"
else
  write_result "failed" "${EXIT_CODE}"
fi

cat "${RESULT_FILE}"
printf '%s\n' '--- test.log (tail) ---'
tail -n 60 "${LOG_FILE}" || true
exit "${EXIT_CODE}"
