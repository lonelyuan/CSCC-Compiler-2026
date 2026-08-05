#!/usr/bin/env bash
# Run a command on the 40-physical-core x86_64 debug host over a MULTIPLEXED ssh
# connection.
#
# Why multiplexing: the host is reached through an frp tunnel on port 6000 that
# rate-limits new TCP connections -- back-to-back `ssh host cmd` invocations fail
# with "Connection closed by ... port 6000" roughly two times out of three. A
# single ControlMaster socket is established once (with retries) and reused by
# every later call, which makes the link reliable and much faster.
#
# Usage:
#   ./scripts/xeon.sh 'nproc'
#   ./scripts/xeon.sh --rsync-args      # print args for rsync -e
set -euo pipefail

XEON_HOST="${XEON_HOST:-}"
XEON_PORT="${XEON_PORT:-6000}"
XEON_USER="${XEON_USER:-ouc}"
SSH_KEY="${SSH_KEY:-${HOME}/.ssh/ouc_xeon_ed25519}"
CM_SOCKET="${CM_SOCKET:-${HOME}/.ssh/cm/xeon}"
CM_PERSIST="${CM_PERSIST:-8h}"
RETRIES="${RETRIES:-12}"

# The host address lives in an untracked local file, not in the repo: it is a
# temporary debug box on a public address, and this repo is mirrored publicly.
# Create scripts/xeon_host.env with `XEON_HOST=<address>` (see xeon_host.env.example).
HOST_ENV="${HOST_ENV:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/xeon_host.env}"
if [[ -z "${XEON_HOST}" && -f "${HOST_ENV}" ]]; then
  # shellcheck disable=SC1090
  source "${HOST_ENV}"
fi
if [[ -z "${XEON_HOST}" ]]; then
  echo "xeon.sh: no host configured. Set XEON_HOST=<address> or create ${HOST_ENV}" >&2
  exit 2
fi

mkdir -p "$(dirname "${CM_SOCKET}")"

common_opts=(
  -o BatchMode=yes
  -o StrictHostKeyChecking=no
  -o UserKnownHostsFile=/dev/null
  -o LogLevel=ERROR
  -i "${SSH_KEY}"
  -p "${XEON_PORT}"
)

ensure_master() {
  if ssh -O check -S "${CM_SOCKET}" "${common_opts[@]}" \
      "${XEON_USER}@${XEON_HOST}" >/dev/null 2>&1; then
    return 0
  fi
  rm -f "${CM_SOCKET}"
  local i
  for ((i = 1; i <= RETRIES; i++)); do
    if ssh -M -S "${CM_SOCKET}" -fN \
        -o ControlPersist="${CM_PERSIST}" \
        -o ServerAliveInterval=30 \
        "${common_opts[@]}" "${XEON_USER}@${XEON_HOST}" 2>/dev/null; then
      return 0
    fi
    sleep $((i < 5 ? 3 : 8))
  done
  echo "xeon.sh: failed to establish ssh control master after ${RETRIES} tries" >&2
  return 1
}

if [[ "${1:-}" == "--rsync-args" ]]; then
  ensure_master
  printf 'ssh -S %s -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -i %s -p %s' \
    "${CM_SOCKET}" "${SSH_KEY}" "${XEON_PORT}"
  exit 0
fi

ensure_master
exec ssh -S "${CM_SOCKET}" "${common_opts[@]}" "${XEON_USER}@${XEON_HOST}" "$@"
