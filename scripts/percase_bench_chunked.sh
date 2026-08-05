#!/usr/bin/env bash
# Run the per-case benchmark over the 150 public cases in memory-bounded chunks,
# then score the union the way the judge does.
#
# Why this exists: tools/percase_harness/main_percase.cpp follows the official
# matrix_case_io contract and holds every input AND every output resident, which
# is 2.46 GB for the 150 public cases. The aarch64 host reached through
# scripts/cg.sh is a container with a hard 2 GiB memory cgroup limit
# (/sys/fs/cgroup/memory/memory.limit_in_bytes, read-only), so a single-process
# run over all 150 cases is OOM-killed at [4/6] with exit 137. Splitting the spec
# into ranges that each fit keeps every measurement faithful and only changes how
# many processes it takes.
#
# What chunking does NOT change: per-case timing is per block_cholesky call, so
# the numbers are directly comparable to a single-process run. What it DOES
# change: any one-time process cost is paid once per chunk instead of once per
# run. Round 13 moved the worker-pool warmup out of the timed region (a discarded
# parallel region at library load), so this should be invisible -- CHECK_BOUNDARY
# re-measures the first case of each chunk inside the previous chunk to confirm.
#
# Usage (on the benchmark host, with the toolchain env loaded):
#   LABEL=r15_aarch64_baseline PASSES=3 ./scripts/percase_bench_chunked.sh
#   CHUNK_BUDGET_MB=400 LABEL=tight ./scripts/percase_bench_chunked.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RESULTS_DIR="${RESULTS_DIR:-${REPO_ROOT}/docs/benchmark_results}"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/build/percase}"
CASE_FILE="${CASE_FILE:-${REPO_ROOT}/contestant_sdk/cases/preliminary_public_150.txt}"

LABEL="${LABEL:-percase_chunked}"
# Resident bytes are input + output for the cases in the chunk. 600 MB of payload
# leaves room for the verifier (which also holds both sides) inside 2 GiB.
CHUNK_BUDGET_MB="${CHUNK_BUDGET_MB:-600}"
M_IDEAL="${M_IDEAL:-32.0}"
STAT="${STAT:-min}"
KEEP_INPUTS="${KEEP_INPUTS:-0}"

mkdir -p "${RESULTS_DIR}" "${WORK_DIR}"

# Chunk boundaries come from the declared case sizes, not from a guess: a chunk
# is closed as soon as adding the next case would exceed the budget.
mapfile -t CHUNKS < <(python3 - "${CASE_FILE}" "${CHUNK_BUDGET_MB}" <<'PY'
import sys

case_file, budget_mb = sys.argv[1], float(sys.argv[2])
budget = budget_mb * 1e6

lines = [l.strip() for l in open(case_file) if l.strip() and not l.strip().startswith("#")]
# Spec lines are n:b:seed, one case each; resident cost is the input matrix plus
# the output matrix, both n*n doubles.
sizes = [2 * int(l.split(":")[0]) ** 2 * 8 for l in lines]

start, total = 1, 0
for index, size in enumerate(sizes, start=1):
    if index > start and total + size > budget:
        print(f"{start} {index - 1}")
        start, total = index, 0
    total += size
print(f"{start} {len(sizes)}")
PY
)

echo "chunking ${#CHUNKS[@]} ranges under ${CHUNK_BUDGET_MB} MB resident:"
for chunk in "${CHUNKS[@]}"; do
  echo "  spec lines ${chunk}"
done
echo

merge_args=()
chunk_index=0
for chunk in "${CHUNKS[@]}"; do
  read -r start end <<< "${chunk}"
  chunk_index=$((chunk_index + 1))
  chunk_label="${LABEL}_c${chunk_index}"
  echo "=== chunk ${chunk_index}/${#CHUNKS[@]}: spec lines ${start}-${end} ==="
  SPEC_START="${start}" SPEC_END="${end}" LABEL="${chunk_label}" \
    M_IDEAL="${M_IDEAL}" STAT="${STAT}" \
    "${SCRIPT_DIR}/percase_bench.sh"
  merge_args+=("${RESULTS_DIR}/${chunk_label}.csv:${start}")
  # The generated input is the bulk of the disk cost (1.2 GB across all chunks)
  # and is not needed once the chunk is scored.
  if [[ "${KEEP_INPUTS}" == "0" ]]; then
    rm -f "${WORK_DIR}/${chunk_label}/input.bin" \
          "${WORK_DIR}/${chunk_label}/serial.out" \
          "${WORK_DIR}/${chunk_label}/contestant.out"
  fi
  echo
done

echo "=== scoring the union of ${#CHUNKS[@]} chunks ==="
python3 "${SCRIPT_DIR}/merge_chunk_csvs.py" \
  --serial-out "${WORK_DIR}/${LABEL}_serial.csv" \
  --contestant-out "${WORK_DIR}/${LABEL}_contestant.csv" \
  "${merge_args[@]}"

python3 "${SCRIPT_DIR}/score_judge.py" \
  "${WORK_DIR}/${LABEL}_serial.csv" \
  "${WORK_DIR}/${LABEL}_contestant.csv" \
  --m-ideal "${M_IDEAL}" --stat "${STAT}" --label "${LABEL}" \
  --merged-csv "${RESULTS_DIR}/${LABEL}.csv"
