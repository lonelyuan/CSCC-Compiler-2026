#!/usr/bin/env bash
# Sync this worktree to the aarch64 performance host (HiSilicon aarch64,
# openEuler 22.03 LTS, 40 cores / 1 NUMA node, 2.9 GHz fixed, BiSheng 3.2.0.1).
# This is the platform to use for any number we report: it matches the judge's
# ISA, OS and toolchain, unlike the x86_64 Xeon debug box.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CG_USER="${CG_USER:-root}"
CG_DIR="${CG_DIR:-/root/bisheng}"

# Host address comes from the untracked scripts/cg_host.env (see cg.sh).
CG_HOST="${CG_HOST:-}"
if [[ -z "${CG_HOST}" && -f "${REPO_ROOT}/scripts/cg_host.env" ]]; then
  # shellcheck disable=SC1091
  source "${REPO_ROOT}/scripts/cg_host.env"
fi
if [[ -z "${CG_HOST}" ]]; then
  echo "sync_to_cg.sh: no host configured. Set CG_HOST or create scripts/cg_host.env" >&2
  exit 2
fi

# Reuse the multiplexed connection from cg.sh so rsync does not pay a second
# tailscale handshake.
RSYNC_SSH="$("${REPO_ROOT}/scripts/cg.sh" --rsync-args)"

rsync -az "$@" \
  -e "${RSYNC_SSH}" \
  --exclude ".DS_Store" \
  --exclude ".git" \
  --exclude "build" \
  --exclude "dist" \
  --exclude "*.pdf" \
  "${REPO_ROOT}/" \
  "${CG_USER}@${CG_HOST}:${CG_DIR}/"

echo "Synced ${REPO_ROOT} to ${CG_USER}@${CG_HOST}:${CG_DIR}"
