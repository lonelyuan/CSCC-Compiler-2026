#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VM_HOST="${VM_HOST:-192.168.8.131}"
VM_USER="${VM_USER:-root}"
VM_DIR="${VM_DIR:-/root/bisheng}"
SSH_KEY="${SSH_KEY:-${HOME}/.ssh/bisheng_vm_ed25519}"

rsync -az --delete \
  -e "ssh -i ${SSH_KEY} -o StrictHostKeyChecking=no" \
  --exclude ".DS_Store" \
  --exclude ".git" \
  --exclude "build" \
  --exclude "dist" \
  "${REPO_ROOT}/" \
  "${VM_USER}@${VM_HOST}:${VM_DIR}/"

echo "Synced ${REPO_ROOT} to ${VM_USER}@${VM_HOST}:${VM_DIR}"

