#!/usr/bin/env bash
# Start a long-running command on the aarch64 performance host DETACHED, and
# stream its log.
#
# Why: a benchmark sweep runs for many minutes, and a laptop that sleeps or roams
# between networks drops the tailscale session under it. `setsid nohup` on the
# remote side means the run survives a dropped connection; we then poll its log
# over the multiplexed connection and can reconnect freely.
#
# Usage:
#   ./scripts/cg_run.sh <job-name> '<remote shell command>'      # start + wait
#   START_ONLY=1 ./scripts/cg_run.sh <job-name> '<command>'      # start only
#   ./scripts/cg_run.sh --tail <job-name>                        # print log
#   ./scripts/cg_run.sh --wait <job-name>                        # wait + print
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CG="${SCRIPT_DIR}/cg.sh"
REMOTE_LOG_DIR="${REMOTE_LOG_DIR:-/root/bisheng-runs}"
POLL_SECONDS="${POLL_SECONDS:-15}"

job_log() { echo "${REMOTE_LOG_DIR}/$1.log"; }
job_done() { echo "${REMOTE_LOG_DIR}/$1.done"; }

wait_for_job() {
  local name="$1"
  local log done_file
  log="$(job_log "${name}")"
  done_file="$(job_done "${name}")"
  while true; do
    if "${CG}" "test -f ${done_file}" 2>/dev/null; then
      break
    fi
    sleep "${POLL_SECONDS}"
  done
  "${CG}" "cat ${log}; echo '--- exit:'; cat ${done_file}"
}

case "${1:-}" in
  --tail)
    exec "${CG}" "tail -n ${TAIL_LINES:-40} $(job_log "$2")"
    ;;
  --wait)
    wait_for_job "$2"
    exit 0
    ;;
esac

name="${1:?job name required}"
command_text="${2:?remote command required}"
log="$(job_log "${name}")"
done_file="$(job_done "${name}")"

"${CG}" "mkdir -p ${REMOTE_LOG_DIR} && rm -f ${log} ${done_file}"
# The remote side writes its exit status to <job>.done so the poller can tell
# "still running" from "finished with a failure".
"${CG}" "setsid nohup bash -lc 'set -o pipefail; { ${command_text} ; } > ${log} 2>&1; echo \$? > ${done_file}' >/dev/null 2>&1 < /dev/null &" || true
echo "started remote job '${name}' -> ${log}"

if [[ -n "${START_ONLY:-}" ]]; then
  exit 0
fi
wait_for_job "${name}"
