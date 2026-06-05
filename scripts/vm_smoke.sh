#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VM_HOST="${VM_HOST:-192.168.8.131}"
VM_USER="${VM_USER:-root}"
VM_DIR="${VM_DIR:-/root/bisheng}"
SSH_KEY="${SSH_KEY:-${HOME}/.ssh/bisheng_vm_ed25519}"
THREADS="${COMPILER2026_DAG_THREADS:-4}"

"${SCRIPT_DIR}/sync_to_vm.sh"

ssh -i "${SSH_KEY}" -o StrictHostKeyChecking=no "${VM_USER}@${VM_HOST}" \
  "source /etc/profile.d/bisheng.sh && cd ${VM_DIR} && COMPILER2026_DAG_THREADS=${THREADS} ./submission/scripts/smoke_test.sh"

