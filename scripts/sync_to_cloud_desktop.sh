#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: sync_to_cloud_desktop.sh [--dry-run]

Sync the local repository source to the authorized AArch64 cloud desktop.
Build output, benchmark data, Git metadata, credentials, and remote-access state
are excluded. The script uses rsync when both endpoints provide it and otherwise
streams a source-only tar archive over SSH.

Environment:
  CLOUD_DESKTOP_HOST         MagicDNS name (default: bisheng-cg-aarch64)
  CLOUD_DESKTOP_USER         SSH user (default: root)
  CLOUD_DESKTOP_DIR          Remote mirror (default: /mnt/cgshare/bisheng)
  CLOUD_DESKTOP_SSH_KEY      Local private-key path (default: ~/.ssh/id_ed25519)
  CLOUD_DESKTOP_SYNC_DELETE  Set to 1 to delete stale files with rsync only
EOF
}

DRY_RUN=0
case "${1:-}" in
  -h|--help)
    usage
    exit 0
    ;;
  -n|--dry-run)
    DRY_RUN=1
    shift
    ;;
esac
if (( $# != 0 )); then
  printf 'unsupported argument: %s\n' "$1" >&2
  exit 64
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CLOUD_HOST="${CLOUD_DESKTOP_HOST:-bisheng-cg-aarch64}"
CLOUD_USER="${CLOUD_DESKTOP_USER:-root}"
CLOUD_DIR="${CLOUD_DESKTOP_DIR:-/mnt/cgshare/bisheng}"
SSH_KEY="${CLOUD_DESKTOP_SSH_KEY:-${HOME}/.ssh/id_ed25519}"

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
if [[ ! "${CLOUD_DIR}" =~ ^/mnt/cgshare/[A-Za-z0-9._/-]+$ ]] || \
   [[ "${CLOUD_DIR}" == "/mnt/cgshare/" ]] || \
   [[ "${CLOUD_DIR}" == "/mnt/cgshare/." || "${CLOUD_DIR}" == *"/./"* || "${CLOUD_DIR}" == */. ]] || \
   [[ "${CLOUD_DIR}" == *"/../"* || "${CLOUD_DIR}" == */.. || "${CLOUD_DIR}" == *"//"* ]]; then
  printf 'CLOUD_DESKTOP_DIR must be below /mnt/cgshare: %s\n' "${CLOUD_DIR}" >&2
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
TAR_EXCLUDES=(
  --exclude "./.DS_Store"
  --exclude "./.git"
  --exclude "./.evaluation"
  --exclude "./.venv"
  --exclude "./.env"
  --exclude "./.env.*"
  --exclude "*/.ssh"
  --exclude "*.pem"
  --exclude "*.key"
  --exclude "*/id_rsa"
  --exclude "*/id_ed25519"
  --exclude "*/__pycache__"
  --exclude "./build"
  --exclude "./build-*"
  --exclude "./dist"
  --exclude "./docs/benchmark_results"
  --exclude "./contestant_sdk/bin"
  --exclude "./contestant_sdk/lib"
  --exclude "*.pdf"
  --exclude "./.cloud-desktop-cookies.txt"
  --exclude "*/tailscale-cloud"
)

if [[ "${DRY_RUN}" == "1" ]]; then
  FILE_COUNT="$(tar -C "${REPO_ROOT}" "${TAR_EXCLUDES[@]}" -cf - . | tar -tf - | wc -l | tr -d ' ')"
  printf 'Dry run: portable source archive contains %s entries; remote was not changed.\n' "${FILE_COUNT}"
  exit 0
fi

REMOTE_BACKEND="$(
  ssh "${SSH_ARGS[@]}" "${CLOUD_USER}@${CLOUD_HOST}" \
    'if command -v rsync >/dev/null 2>&1; then printf rsync; elif command -v tar >/dev/null 2>&1; then printf tar; else printf none; fi'
)"
if [[ "${REMOTE_BACKEND}" == "none" ]]; then
  printf 'remote host has neither rsync nor tar\n' >&2
  exit 69
fi
if [[ "${CLOUD_DESKTOP_SYNC_DELETE:-0}" == "1" && "${REMOTE_BACKEND}" != "rsync" ]]; then
  printf 'remote deletion requires rsync on the cloud desktop; refusing tar fallback\n' >&2
  exit 69
fi

ssh "${SSH_ARGS[@]}" "${CLOUD_USER}@${CLOUD_HOST}" \
  "mkdir -p -- '${CLOUD_DIR}'"

if [[ "${REMOTE_BACKEND}" == "rsync" ]] && command -v rsync >/dev/null 2>&1; then
  printf -v SSH_CMD '%q ' ssh "${SSH_ARGS[@]}"
  RSYNC_ARGS=(-az)
  if [[ "${CLOUD_DESKTOP_SYNC_DELETE:-0}" == "1" ]]; then
    RSYNC_ARGS+=(--delete-delay)
  fi
  rsync "${RSYNC_ARGS[@]}" \
    -e "${SSH_CMD}" \
    --exclude ".DS_Store" \
    --exclude ".git" \
    --exclude ".evaluation" \
    --exclude ".venv" \
    --exclude ".env" \
    --exclude ".env.*" \
    --exclude ".ssh" \
    --exclude "*.pem" \
    --exclude "*.key" \
    --exclude "id_rsa" \
    --exclude "id_ed25519" \
    --exclude "__pycache__" \
    --exclude "build" \
    --exclude "build-*" \
    --exclude "dist" \
    --exclude "docs/benchmark_results" \
    --exclude "contestant_sdk/bin" \
    --exclude "contestant_sdk/lib" \
    --exclude "*.pdf" \
    --exclude ".cloud-desktop-cookies.txt" \
    --exclude "tailscale-cloud" \
    "${REPO_ROOT}/" \
    "${CLOUD_USER}@${CLOUD_HOST}:${CLOUD_DIR}/"
  SYNC_BACKEND=rsync
else
  if [[ "${CLOUD_DESKTOP_SYNC_DELETE:-0}" == "1" ]]; then
    printf 'local rsync is unavailable; refusing delete-enabled tar fallback\n' >&2
    exit 69
  fi
  tar -C "${REPO_ROOT}" "${TAR_EXCLUDES[@]}" -czf - . | \
    ssh "${SSH_ARGS[@]}" "${CLOUD_USER}@${CLOUD_HOST}" \
      "tar -xzf - -C '${CLOUD_DIR}'"
  SYNC_BACKEND=tar-over-ssh
fi

SOURCE_COMMIT="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
if [[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]]; then
  SOURCE_DIRTY=1
else
  SOURCE_DIRTY=0
fi
SOURCE_SYNCED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

ssh "${SSH_ARGS[@]}" "${CLOUD_USER}@${CLOUD_HOST}" \
  "mkdir -p -- '${CLOUD_DIR}/.evaluation' && printf '%s\\n' 'source_commit=${SOURCE_COMMIT}' 'source_dirty=${SOURCE_DIRTY}' 'synced_at_utc=${SOURCE_SYNCED_AT}' 'sync_backend=${SYNC_BACKEND}' > '${CLOUD_DIR}/.evaluation/source-state.env'"

printf 'Synced %s to %s@%s:%s with %s\n' \
  "${REPO_ROOT}" "${CLOUD_USER}" "${CLOUD_HOST}" "${CLOUD_DIR}" "${SYNC_BACKEND}"
printf 'Source state: commit=%s dirty=%s metadata=%s/.evaluation/source-state.env\n' \
  "${SOURCE_COMMIT}" "${SOURCE_DIRTY}" "${CLOUD_DIR}"
if [[ "${SOURCE_DIRTY}" == "1" ]]; then
  printf 'Warning: the synced tree is dirty; the commit ID alone is not reproducible.\n' >&2
fi
