#!/usr/bin/env bash
# Sync this worktree to the 40-physical-core x86_64 debug host (Xeon Gold 5218R x2,
# Ubuntu 22.04, LLVM 17). That host is NOT the official performance platform
# (Kunpeng 920 / openEuler / BiSheng); it is used for structural scheduling
# experiments and same-host relative comparisons only.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XEON_USER="${XEON_USER:-ouc}"
XEON_DIR="${XEON_DIR:-/home/ouc/bisheng}"

# Host address comes from the untracked scripts/xeon_host.env (see xeon.sh).
XEON_HOST="${XEON_HOST:-}"
if [[ -z "${XEON_HOST}" && -f "${REPO_ROOT}/scripts/xeon_host.env" ]]; then
  # shellcheck disable=SC1091
  source "${REPO_ROOT}/scripts/xeon_host.env"
fi
if [[ -z "${XEON_HOST}" ]]; then
  echo "sync_to_xeon.sh: no host configured. Set XEON_HOST or create scripts/xeon_host.env" >&2
  exit 2
fi

# Reuse the multiplexed connection from xeon.sh: the frp tunnel rate-limits new
# TCP connections, so a fresh rsync ssh session fails most of the time.
RSYNC_SSH="$("${REPO_ROOT}/scripts/xeon.sh" --rsync-args)"

rsync -az "$@" \
  -e "${RSYNC_SSH}" \
  --exclude ".DS_Store" \
  --exclude ".git" \
  --exclude "build" \
  --exclude "dist" \
  --exclude "*.pdf" \
  "${REPO_ROOT}/" \
  "${XEON_USER}@${XEON_HOST}:${XEON_DIR}/"

echo "Synced ${REPO_ROOT} to ${XEON_USER}@${XEON_HOST}:${XEON_DIR}"
