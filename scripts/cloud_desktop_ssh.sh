#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: cloud_desktop_ssh.sh [--check] [--] [remote command...]

Connect to the authorized CourseGrading AArch64 cloud desktop over its private
Tailnet SSH path. With no command, open an interactive shell.

Environment:
  CLOUD_DESKTOP_HOST      MagicDNS name (default: bisheng-cg-aarch64)
  CLOUD_DESKTOP_USER      SSH user (default: root)
  CLOUD_DESKTOP_SSH_KEY   Local private-key path (default: ~/.ssh/id_ed25519)
EOF
}

CLOUD_HOST="${CLOUD_DESKTOP_HOST:-bisheng-cg-aarch64}"
CLOUD_USER="${CLOUD_DESKTOP_USER:-root}"
SSH_KEY="${CLOUD_DESKTOP_SSH_KEY:-${HOME}/.ssh/id_ed25519}"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ ! -r "${SSH_KEY}" ]]; then
  printf 'cloud desktop SSH key is not readable: %s\n' "${SSH_KEY}" >&2
  exit 66
fi
if [[ ! "${CLOUD_HOST}" =~ ^[A-Za-z0-9._-]+$ ]]; then
  printf 'invalid CLOUD_DESKTOP_HOST: %s\n' "${CLOUD_HOST}" >&2
  exit 64
fi
if [[ ! "${CLOUD_USER}" =~ ^[A-Za-z0-9._-]+$ ]]; then
  printf 'invalid CLOUD_DESKTOP_USER: %s\n' "${CLOUD_USER}" >&2
  exit 64
fi

SSH_ARGS=(
  -o BatchMode=yes
  -o ConnectTimeout=15
  -o ServerAliveInterval=30
  -o ServerAliveCountMax=3
  -o StrictHostKeyChecking=accept-new
  -o IdentitiesOnly=yes
  -i "${SSH_KEY}"
)

if [[ "${1:-}" == "--check" ]]; then
  if command -v tailscale >/dev/null 2>&1; then
    tailscale ping --c 1 --timeout=10s "${CLOUD_HOST}" || true
  fi
  exec ssh "${SSH_ARGS[@]}" "${CLOUD_USER}@${CLOUD_HOST}" \
    'printf "arch="; uname -m; printf "nproc="; nproc; printf "cwd="; pwd'
fi

if [[ "${1:-}" == "--" ]]; then
  shift
fi

if (( $# == 0 )); then
  exec ssh "${SSH_ARGS[@]}" "${CLOUD_USER}@${CLOUD_HOST}"
fi

# Pass each remaining argument directly to ssh. Callers that need shell syntax
# should provide one explicitly quoted command string.
exec ssh "${SSH_ARGS[@]}" "${CLOUD_USER}@${CLOUD_HOST}" "$@"
