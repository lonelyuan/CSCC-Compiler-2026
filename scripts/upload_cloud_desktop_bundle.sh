#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: upload_cloud_desktop_bundle.sh <desktop-url> <bundle.zip> [response-file]

Upload a generated cloud-desktop test bundle using the short-lived cicdParam
from a freshly supplied CourseGrading desktop URL. The URL is never printed or
written to disk. This script uploads only; no contest submission is involved.

Environment:
  CLOUD_DESKTOP_COOKIE_FILE  Optional user-provided Netscape cookie file for
                             the same logged-in CourseGrading session.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" || "$#" -lt 2 || "$#" -gt 3 ]]; then
  usage >&2
  exit 64
fi

DESKTOP_URL="$1"
BUNDLE_PATH="$2"
RESPONSE_PATH="${3:-${BUNDLE_PATH%.zip}.upload-response.html}"

if [[ ! "${DESKTOP_URL}" =~ ^https://course\.educg\.net/authincludes/expEnv/doexpDeskDocker\.jsp\? ]]; then
  printf '%s\n' 'desktop URL must be a fresh CourseGrading doexpDeskDocker.jsp URL' >&2
  exit 64
fi
if [[ ! -f "${BUNDLE_PATH}" ]]; then
  printf 'bundle does not exist: %s\n' "${BUNDLE_PATH}" >&2
  exit 66
fi

BUNDLE_NAME="$(basename "${BUNDLE_PATH}")"
if [[ ! "${BUNDLE_NAME}" =~ ^[A-Za-z0-9._-]+$ ]]; then
  printf 'bundle filename must use only A-Z, a-z, 0-9, ., _, -: %s\n' "${BUNDLE_NAME}" >&2
  exit 64
fi

BUNDLE_SIZE="$(wc -c < "${BUNDLE_PATH}" | tr -d '[:space:]')"
MAX_SIZE=$((64 * 1024 * 1024))
if (( BUNDLE_SIZE > MAX_SIZE )); then
  printf 'bundle exceeds cloud desktop limit: %s bytes > %s bytes\n' \
    "${BUNDLE_SIZE}" "${MAX_SIZE}" >&2
  exit 65
fi

QUERY="${DESKTOP_URL#*\?}"
CICD_PARAM=""
IFS='&' read -r -a QUERY_PAIRS <<< "${QUERY}"
for pair in "${QUERY_PAIRS[@]}"; do
  if [[ "${pair}" == cicdParam=* ]]; then
    CICD_PARAM="${pair#cicdParam=}"
    break
  fi
done
unset QUERY_PAIRS QUERY pair

if [[ -z "${CICD_PARAM}" ]]; then
  printf '%s\n' 'desktop URL has no cicdParam' >&2
  exit 64
fi

UPLOAD_URL="https://course.educg.net/authincludes/expEnv/doexpDeskDockerUpload.jsp?doUpload=true&cicdParam=${CICD_PARAM}"
RESPONSE_TMP="${RESPONSE_PATH}.tmp"
trap 'rm -f "${RESPONSE_TMP}"' EXIT

CURL_ARGS=(
  --silent
  --show-error
  --output "${RESPONSE_TMP}"
  --write-out '%{http_code}'
  --request POST
  --header 'Origin: https://course.educg.net'
  --header 'Referer: https://course.educg.net/authincludes/expEnv/doexpDeskDocker.jsp'
  --header 'X-Requested-With: XMLHttpRequest'
  --header 'Accept: */*'
)
if [[ -n "${CLOUD_DESKTOP_COOKIE_FILE:-}" ]]; then
  if [[ ! -f "${CLOUD_DESKTOP_COOKIE_FILE}" ]]; then
    printf 'cookie file does not exist: %s\n' "${CLOUD_DESKTOP_COOKIE_FILE}" >&2
    exit 66
  fi
  CURL_ARGS+=(--cookie "${CLOUD_DESKTOP_COOKIE_FILE}")
fi

CURL_ARGS+=(
  --form "file=@${BUNDLE_PATH};filename=${BUNDLE_NAME}"
  "${UPLOAD_URL}"
)
HTTP_STATUS="$(curl "${CURL_ARGS[@]}")"
unset CURL_ARGS

mv "${RESPONSE_TMP}" "${RESPONSE_PATH}"
trap - EXIT
unset CICD_PARAM UPLOAD_URL DESKTOP_URL

printf 'upload_http_status=%s\n' "${HTTP_STATUS}"
printf 'upload_response=%s\n' "${RESPONSE_PATH}"
if [[ ! "${HTTP_STATUS}" =~ ^2 ]]; then
  printf '%s\n' 'upload was not accepted; the endpoint requires the matching logged-in browser session.' >&2
  exit 69
fi
if [[ ! -s "${RESPONSE_PATH}" ]]; then
  printf '%s\n' 'upload response was empty; upload is unverified and must not be treated as successful.' >&2
  exit 69
fi
