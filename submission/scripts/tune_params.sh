#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUBMISSION_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${SUBMISSION_DIR}/.." && pwd)"
BENCH_DIR="${BENCH_DIR:-${REPO_ROOT}/build/optimization_benchmarks}"

LABEL_PREFIX="${COMPILER2026_TUNE_LABEL_PREFIX:-param_sweep}"
THREAD_LIST="${COMPILER2026_TUNE_THREAD_LIST:-${COMPILER2026_DAG_THREAD_LIST:-${COMPILER2026_DAG_THREADS:-1,2,4}}}"
ASYNC_MIN_B_LIST="${COMPILER2026_TUNE_ASYNC_MIN_B_LIST:-18,24,32,48}"
ASYNC_MIN_BLOCKS_LIST="${COMPILER2026_TUNE_ASYNC_MIN_BLOCKS_LIST:-${COMPILER2026_ASYNC_MIN_BLOCKS:-2}}"
TASK_BATCH_LIST="${COMPILER2026_TUNE_TASK_BATCH_LIST:-auto,4,8}"
DAG_MAX_LIVE_LIST="${COMPILER2026_TUNE_DAG_MAX_LIVE_LIST:-${COMPILER2026_DAG_MAX_LIVE:-0}}"
REPEAT="${REPEAT:-1}"
PROFILE="${COMPILER2026_DAG_PROFILE:-${COMPILER2026_TUNE_PROFILE:-0}}"
DRY_RUN="${COMPILER2026_TUNE_DRY_RUN:-0}"

parse_list() {
  local raw="$1"
  raw="${raw//,/ }"
  read -r -a PARSED_LIST <<< "${raw}"
}

require_positive_int() {
  local name="$1"
  local value="$2"
  if [[ ! "${value}" =~ ^[0-9]+$ || "${value}" == "0" ]]; then
    echo "invalid ${name} entry: ${value}" >&2
    exit 2
  fi
}

require_nonnegative_int() {
  local name="$1"
  local value="$2"
  if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
    echo "invalid ${name} entry: ${value}" >&2
    exit 2
  fi
}

sanitize_label_part() {
  local value="$1"
  value="${value//[^A-Za-z0-9._-]/_}"
  printf '%s' "${value}"
}

parse_list "${THREAD_LIST}"
THREAD_VALUES=("${PARSED_LIST[@]}")
parse_list "${ASYNC_MIN_B_LIST}"
ASYNC_MIN_B_VALUES=("${PARSED_LIST[@]}")
parse_list "${ASYNC_MIN_BLOCKS_LIST}"
ASYNC_MIN_BLOCKS_VALUES=("${PARSED_LIST[@]}")
parse_list "${TASK_BATCH_LIST}"
TASK_BATCH_VALUES=("${PARSED_LIST[@]}")
parse_list "${DAG_MAX_LIVE_LIST}"
DAG_MAX_LIVE_VALUES=("${PARSED_LIST[@]}")

if [[ "${#THREAD_VALUES[@]}" -eq 0 ]]; then
  echo "empty COMPILER2026_TUNE_THREAD_LIST" >&2
  exit 2
fi
if [[ "${#ASYNC_MIN_B_VALUES[@]}" -eq 0 ]]; then
  echo "empty COMPILER2026_TUNE_ASYNC_MIN_B_LIST" >&2
  exit 2
fi
if [[ "${#TASK_BATCH_VALUES[@]}" -eq 0 ]]; then
  echo "empty COMPILER2026_TUNE_TASK_BATCH_LIST" >&2
  exit 2
fi
if [[ "${#ASYNC_MIN_BLOCKS_VALUES[@]}" -eq 0 ]]; then
  echo "empty COMPILER2026_TUNE_ASYNC_MIN_BLOCKS_LIST" >&2
  exit 2
fi
if [[ "${#DAG_MAX_LIVE_VALUES[@]}" -eq 0 ]]; then
  echo "empty COMPILER2026_TUNE_DAG_MAX_LIVE_LIST" >&2
  exit 2
fi

for thread_value in "${THREAD_VALUES[@]}"; do
  require_positive_int "COMPILER2026_TUNE_THREAD_LIST" "${thread_value}"
done
for min_b in "${ASYNC_MIN_B_VALUES[@]}"; do
  require_positive_int "COMPILER2026_TUNE_ASYNC_MIN_B_LIST" "${min_b}"
done
for min_blocks in "${ASYNC_MIN_BLOCKS_VALUES[@]}"; do
  require_positive_int "COMPILER2026_TUNE_ASYNC_MIN_BLOCKS_LIST" "${min_blocks}"
done
for batch in "${TASK_BATCH_VALUES[@]}"; do
  if [[ "${batch}" != "auto" ]]; then
    require_positive_int "COMPILER2026_TUNE_TASK_BATCH_LIST" "${batch}"
  fi
done
for max_live in "${DAG_MAX_LIVE_VALUES[@]}"; do
  require_nonnegative_int "COMPILER2026_TUNE_DAG_MAX_LIVE_LIST" "${max_live}"
done

mkdir -p "${BENCH_DIR}"
SAFE_PREFIX="$(sanitize_label_part "${LABEL_PREFIX}")"
AGGREGATE_CSV="${BENCH_DIR}/${SAFE_PREFIX}_aggregate.csv"
rm -f "${AGGREGATE_CSV}"

THREADS_JOINED="$(IFS=,; echo "${THREAD_VALUES[*]}")"

echo "tune_label_prefix=${SAFE_PREFIX}"
echo "threads=${THREADS_JOINED}"
echo "async_min_b_list=$(IFS=,; echo "${ASYNC_MIN_B_VALUES[*]}")"
echo "async_min_blocks_list=$(IFS=,; echo "${ASYNC_MIN_BLOCKS_VALUES[*]}")"
echo "task_batch_list=$(IFS=,; echo "${TASK_BATCH_VALUES[*]}")"
echo "dag_max_live_list=$(IFS=,; echo "${DAG_MAX_LIVE_VALUES[*]}")"
echo "repeat=${REPEAT}"
echo "profile=${PROFILE}"
echo "aggregate_csv=${AGGREGATE_CSV}"

for min_b in "${ASYNC_MIN_B_VALUES[@]}"; do
  for min_blocks in "${ASYNC_MIN_BLOCKS_VALUES[@]}"; do
    for max_live in "${DAG_MAX_LIVE_VALUES[@]}"; do
      for batch in "${TASK_BATCH_VALUES[@]}"; do
        batch_label="$(sanitize_label_part "${batch}")"
        label="${SAFE_PREFIX}_b${min_b}_blocks${min_blocks}_live${max_live}_batch${batch_label}"
        csv="${BENCH_DIR}/${label}.csv"

        echo "running label=${label} async_min_b=${min_b} async_min_blocks=${min_blocks} dag_max_live=${max_live} task_batch=${batch} threads=${THREADS_JOINED}"
        if [[ "${DRY_RUN}" != "0" ]]; then
          continue
        fi

        COMPILER2026_DAG_THREAD_LIST="${THREADS_JOINED}" \
          COMPILER2026_ASYNC_MIN_B="${min_b}" \
          COMPILER2026_ASYNC_MIN_BLOCKS="${min_blocks}" \
          COMPILER2026_TASK_BATCH="${batch}" \
          COMPILER2026_DAG_PROFILE="${PROFILE}" \
          COMPILER2026_DAG_MAX_LIVE="${max_live}" \
          LABEL="${label}" \
          REPEAT="${REPEAT}" \
          "${SCRIPT_DIR}/benchmark.sh"

        if [[ ! -f "${csv}" ]]; then
          echo "missing expected benchmark csv: ${csv}" >&2
          exit 1
        fi

        if [[ ! -f "${AGGREGATE_CSV}" ]]; then
          cp "${csv}" "${AGGREGATE_CSV}"
        else
          tail -n +2 "${csv}" >> "${AGGREGATE_CSV}"
        fi
      done
    done
  done
done

if [[ "${DRY_RUN}" == "0" ]]; then
  python3 - "${AGGREGATE_CSV}" <<'PY'
import csv
import math
import statistics
import sys

path = sys.argv[1]
rows = list(csv.DictReader(open(path)))
print(f"aggregate_csv={path}")

def sort_key(value):
    try:
        return (0, int(value))
    except ValueError:
        return (1, value)

groups = {}
for row in rows:
    key = (
        row["async_min_b"],
        row.get("async_min_blocks", "2"),
        row.get("dag_max_live", "0"),
        row.get("dag_pin_workers", "0"),
        row["task_batch"],
        row["threads"],
    )
    groups.setdefault(key, []).append(row)

for async_min_b, async_min_blocks, dag_max_live, dag_pin_workers, task_batch, threads in sorted(
    groups,
    key=lambda item: (
        sort_key(item[5]),
        sort_key(item[0]),
        sort_key(item[1]),
        sort_key(item[2]),
        sort_key(item[3]),
        sort_key(item[4]),
    ),
):
    group_rows = groups[(
        async_min_b,
        async_min_blocks,
        dag_max_live,
        dag_pin_workers,
        task_batch,
        threads,
    )]
    speedups = [float(row["speedup"]) for row in group_rows]
    positive = [value for value in speedups if value > 0]
    speedup_geo = math.exp(statistics.mean(math.log(value) for value in positive)) if positive else 0.0
    serial_total = sum(float(row["serial_seconds"]) for row in group_rows)
    contestant_total = sum(float(row["contestant_seconds"]) for row in group_rows)
    missing_deps = sum(int(row.get("dag_missing_deps") or 0) for row in group_rows)
    print(
        "tune_summary: "
        f"threads={threads} "
        f"async_min_b={async_min_b} "
        f"async_min_blocks={async_min_blocks} "
        f"dag_max_live={dag_max_live} "
        f"dag_pin_workers={dag_pin_workers} "
        f"task_batch={task_batch} "
        f"runs={len(group_rows)} "
        f"serial_total={serial_total:.6f}s "
        f"contestant_total={contestant_total:.6f}s "
        f"speedup_geo={speedup_geo:.3f}x "
        f"dag_missing_deps={missing_deps}"
    )
PY
fi
