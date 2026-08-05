#!/usr/bin/env bash
# Run a command on the aarch64 performance host over a MULTIPLEXED ssh
# connection reached through the tailscale mesh.
#
# This host is the closest stand-in for the judge platform we have: HiSilicon
# aarch64, openEuler 22.03 LTS, 40 cores in a single NUMA node, frequency pinned
# at 2.9 GHz (boost disabled), BiSheng 3.2.0.1 (clang 15.0.4) under /opt/bisheng.
# Prefer it over the x86_64 Xeon box (scripts/xeon.sh) for anything that claims a
# performance number.
#
# Why multiplexing: every call pays the tailscale handshake otherwise. One
# ControlMaster socket is established once and reused by every later call.
#
# Usage:
#   ./scripts/cg.sh 'nproc'
#   ./scripts/cg.sh --rsync-args      # print args for rsync -e
set -euo pipefail

CG_HOST="${CG_HOST:-}"
CG_PORT="${CG_PORT:-22}"
CG_USER="${CG_USER:-root}"
SSH_KEY="${SSH_KEY:-${HOME}/.ssh/id_ed25519}"
CM_SOCKET="${CM_SOCKET:-${HOME}/.ssh/cm/cg}"
CM_PERSIST="${CM_PERSIST:-8h}"
RETRIES="${RETRIES:-6}"

# The host address lives in an untracked local file, not in the repo: this repo
# is mirrored publicly. Create scripts/cg_host.env with `CG_HOST=<address>`
# (see cg_host.env.example).
HOST_ENV="${HOST_ENV:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/cg_host.env}"
if [[ -z "${CG_HOST}" && -f "${HOST_ENV}" ]]; then
  # shellcheck disable=SC1090
  source "${HOST_ENV}"
fi
if [[ -z "${CG_HOST}" ]]; then
  echo "cg.sh: no host configured. Set CG_HOST=<address> or create ${HOST_ENV}" >&2
  exit 2
fi

mkdir -p "$(dirname "${CM_SOCKET}")"

common_opts=(
  -o BatchMode=yes
  -o StrictHostKeyChecking=no
  -o UserKnownHostsFile=/dev/null
  -o LogLevel=ERROR
  -i "${SSH_KEY}"
  -p "${CG_PORT}"
)

ensure_master() {
  if ssh -O check -S "${CM_SOCKET}" "${common_opts[@]}" \
      "${CG_USER}@${CG_HOST}" >/dev/null 2>&1; then
    return 0
  fi
  rm -f "${CM_SOCKET}"
  local i
  for ((i = 1; i <= RETRIES; i++)); do
    if ssh -M -S "${CM_SOCKET}" -fN \
        -o ControlPersist="${CM_PERSIST}" \
        -o ServerAliveInterval=30 \
        "${common_opts[@]}" "${CG_USER}@${CG_HOST}" 2>/dev/null; then
      return 0
    fi
    sleep $((i < 3 ? 2 : 5))
  done
  echo "cg.sh: failed to establish ssh control master after ${RETRIES} tries" >&2
  return 1
}

if [[ "${1:-}" == "--rsync-args" ]]; then
  ensure_master
  printf 'ssh -S %s -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -i %s -p %s' \
    "${CM_SOCKET}" "${SSH_KEY}" "${CG_PORT}"
  exit 0
fi

ensure_master
exec ssh -S "${CM_SOCKET}" "${common_opts[@]}" "${CG_USER}@${CG_HOST}" "$@"
