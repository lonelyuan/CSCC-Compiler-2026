#!/usr/bin/env bash
# Experiment 1, part A: attribute "more cores makes small tiles slower" to work
# that got slower (memory) or to time spent outside the work (dispatch/sync).
#
# The target container denies perf_event_open outright -- even software events --
# so cache-miss counting is not available (see DEVELOPMENT_GUIDE.md 2.1). This
# probe uses the discriminator the runtime already carries instead, which is
# sharper than a miss rate anyway because it is a direct time split:
#
#   exec_ns  is accumulated INSIDE runBatch, around the task body only, with no
#            runtime lock held. It is time spent in the official kernels.
#   queue_ns is enqueue -> dequeue latency, and worker_idle_ns is condvar wait.
#
# So, holding total flops fixed and raising the participant count T:
#   exec_ns rises      -> each madd itself got slower -> memory/coherence bound.
#                         Adding cores cannot help; raise arithmetic intensity.
#   exec_ns flat, wall
#   time rises         -> the extra time is outside the kernels -> dispatch,
#                         barrier or queue contention. Fix the scheduler.
#
# Profiling itself costs two clock_gettime calls per task plus one locked
# update per batch, so the PROFILED wall time is not a performance number. Every
# point is therefore measured twice: once with the profile on (for the time
# split) and once with it off (for the wall time and the verifier).
#
# Usage (on the benchmark host, toolchain env loaded):
#   CASES="1152:8 1152:12 1152:32" THREADS="8 16 24 32 40" \
#     CSV_OUT=/root/bisheng/r16_exec_attrib.csv ./scripts/exec_attrib_probe.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SUBMISSION_DIR="${REPO_ROOT}/submission"
SDK_DIR="${REPO_ROOT}/contestant_sdk"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/submission_bisheng}"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/build/exec_attrib}"

CXX_BIN="${CXX:-/opt/bisheng/bin/clang++}"
OPT_BIN="${OPT:-/opt/bisheng/bin/opt}"

CASES="${CASES:-1152:8 1152:12 1152:32}"
THREADS="${THREADS:-8 16 24 32 40}"
REPEAT="${REPEAT:-3}"
# Participant cap off by default: this probe is about what T itself does, and
# the shipped cap would clamp the high end and hide the effect.
PIN="${PIN:-COMPILER2026_DAG_PARTICIPANT_CAP=off}"
CSV_OUT="${CSV_OUT:-${WORK_DIR}/exec_attrib.csv}"
TASKSET_CPUS="${TASKSET_CPUS:-0-39}"
if [[ -n "${TASKSET_CPUS}" ]] && command -v taskset >/dev/null 2>&1; then
  RUNNER=(taskset -c "${TASKSET_CPUS}")
else
  RUNNER=()
fi

HARNESS_MAIN="${REPO_ROOT}/tools/percase_harness/main_percase.cpp"
BLOCK_CHOLESKY="${SUBMISSION_DIR}/src/baseline/block_cholesky.cpp"

echo "[1/3] building"
"${SUBMISSION_DIR}/scripts/build.sh" >/dev/null
mkdir -p "${WORK_DIR}/bin" "${WORK_DIR}/ir" "${WORK_DIR}/log"
cd "${SDK_DIR}"

COMMON_CXX=(-std=c++17 -O2 -pthread -Iinclude -Isrc/base_kernels)
KERNELS=(src/base_kernels/kernels_public.cpp src/base_kernels/kernels_impl.cpp)

"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_generator/spd_generator.cpp -o "${WORK_DIR}/bin/spd_generator"
"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_verifier/verifier.cpp -o "${WORK_DIR}/bin/verifier"
"${CXX_BIN}" "${COMMON_CXX[@]}" "${KERNELS[@]}" "${HARNESS_MAIN}" \
  "${BLOCK_CHOLESKY}" -o "${WORK_DIR}/bin/serial"
"${CXX_BIN}" -std=c++17 -O2 -Iinclude -Isrc/base_kernels \
  -emit-llvm -c "${BLOCK_CHOLESKY}" -o "${WORK_DIR}/ir/bc.bc"
"${OPT_BIN}" -load-pass-plugin "${BUILD_DIR}/pass/libcontestant_pass.so" \
  -passes=contestant-pass "${WORK_DIR}/ir/bc.bc" -o "${WORK_DIR}/ir/bc.opt.bc"
"${CXX_BIN}" "${COMMON_CXX[@]}" "${WORK_DIR}/ir/bc.opt.bc" "${KERNELS[@]}" \
  "${HARNESS_MAIN}" "${BUILD_DIR}/runtime/libcontestant_runtime.a" \
  -o "${WORK_DIR}/bin/contestant"

read -r -a PIN_ARGS <<< "${PIN}"

echo "n,b,threads,serial_s,wall_s,speedup,prof_wall_s,tasks,dequeue_batches,exec_ms,queue_ms,worker_idle_ms,main_wait_ms,ready_samples,ready_sum,madd_count,madd_exec_ms,madd_queue_ms,trsm_count,trsm_exec_ms,trsm_queue_ms" > "${CSV_OUT}"

echo "[2/3] sweeping T over ${CASES}"
printf "  %-12s %-8s %-9s %-9s %-10s %-11s %-11s %-10s\n" \
  case T speedup wall_s exec_ms idle_ms queue_ms tasks

for case_spec in ${CASES}; do
  n="${case_spec%%:*}"
  b="${case_spec##*:}"
  case_dir="${WORK_DIR}/${n}_${b}"
  mkdir -p "${case_dir}"
  printf "%s:%s:1\n" "${n}" "${b}" > "${case_dir}/spec.txt"
  if [[ ! -f "${case_dir}/input.bin" ]]; then
    SPD_GENERATOR_THREADS="${GENERATOR_THREADS:-40}" \
      "${WORK_DIR}/bin/spd_generator" "${case_dir}/input.bin" \
      --spec "${case_dir}/spec.txt" >/dev/null
  fi

  COMPILER2026_PERCASE_CSV="${case_dir}/serial.csv" \
  COMPILER2026_PERCASE_REPEAT="${REPEAT}" \
    "${RUNNER[@]}" "${WORK_DIR}/bin/serial" \
    "${case_dir}/input.bin" "${case_dir}/serial.out" >/dev/null
  serial=$(awk -F, 'NR==2 {print $5}' "${case_dir}/serial.csv")

  for t in ${THREADS}; do
    # Pass 1: profiling OFF -- this is the wall time and the verified run.
    COMPILER2026_PERCASE_CSV="${case_dir}/w_${t}.csv" \
    COMPILER2026_PERCASE_REPEAT="${REPEAT}" \
      env "COMPILER2026_DAG_THREADS=${t}" ${PIN_ARGS[@]+"${PIN_ARGS[@]}"} \
      "${RUNNER[@]}" "${WORK_DIR}/bin/contestant" \
      "${case_dir}/input.bin" "${case_dir}/out_${t}.bin" >/dev/null
    wall=$(awk -F, 'NR==2 {print $5}' "${case_dir}/w_${t}.csv")
    VERIFIER_THREADS="${VERIFIER_THREADS:-40}" \
      "${WORK_DIR}/bin/verifier" "${case_dir}/input.bin" "${case_dir}/out_${t}.bin" \
      > "${case_dir}/v_${t}.txt"
    if ! grep -q "status=PASS" "${case_dir}/v_${t}.txt"; then
      echo "  n=${n} b=${b} T=${t}: VERIFIER FAIL" >&2
      continue
    fi
    rm -f "${case_dir}/out_${t}.bin"

    # Pass 2: profiling ON -- REPEAT=1 so the counters describe one call.
    log="${WORK_DIR}/log/${n}_${b}_t${t}.log"
    COMPILER2026_PERCASE_CSV="${case_dir}/p_${t}.csv" \
    COMPILER2026_PERCASE_REPEAT=1 \
      env "COMPILER2026_DAG_THREADS=${t}" "COMPILER2026_DAG_PROFILE=1" \
      ${PIN_ARGS[@]+"${PIN_ARGS[@]}"} \
      "${RUNNER[@]}" "${WORK_DIR}/bin/contestant" \
      "${case_dir}/input.bin" "${case_dir}/out_p_${t}.bin" 2> "${log}" >/dev/null
    prof_wall=$(awk -F, 'NR==2 {print $5}' "${case_dir}/p_${t}.csv")
    rm -f "${case_dir}/out_p_${t}.bin"

    read -r tasks dq exec_ms queue_ms idle_ms main_wait rs rsum <<< "$(
      awk '/^\[compiler2026_profile\]/ {
             for (i = 1; i <= NF; ++i) { split($i, kv, "="); v[kv[1]] = kv[2] }
           }
           END { printf "%s %s %s %s %s %s %s %s",
                        v["tasks"], v["dequeue_batches"], v["exec_ms"], v["queue_ms"],
                        v["worker_idle_ms"], v["main_wait_ms"], v["ready_samples"],
                        v["ready_sum"] }' "${log}")"

    read -r madd_c madd_e madd_q <<< "$(
      awk '/^\[compiler2026_profile_task\]/ {
             for (i = 1; i <= NF; ++i) { split($i, kv, "="); v[kv[1]] = kv[2] }
             if (v["name"] ~ /madd/) { c += v["count"]; e += v["exec_ms"]; q += v["queue_ms"] }
           }
           END { printf "%d %.3f %.3f", c, e, q }' "${log}")"
    read -r trsm_c trsm_e trsm_q <<< "$(
      awk '/^\[compiler2026_profile_task\]/ {
             for (i = 1; i <= NF; ++i) { split($i, kv, "="); v[kv[1]] = kv[2] }
             if (v["name"] == "trsm") { c += v["count"]; e += v["exec_ms"]; q += v["queue_ms"] }
           }
           END { printf "%d %.3f %.3f", c, e, q }' "${log}")"

    speedup=$(awk -v s="${serial}" -v c="${wall}" 'BEGIN{printf "%.4f", s/c}')
    echo "${n},${b},${t},${serial},${wall},${speedup},${prof_wall},${tasks},${dq},${exec_ms},${queue_ms},${idle_ms},${main_wait},${rs},${rsum},${madd_c},${madd_e},${madd_q},${trsm_c},${trsm_e},${trsm_q}" \
      >> "${CSV_OUT}"
    printf "  %-12s %-8s %-9s %-9s %-10s %-11s %-11s %-10s\n" \
      "n=${n} b=${b}" "${t}" "$(awk -v v="${speedup}" 'BEGIN{printf "%.2fx", v}')" \
      "${wall}" "${exec_ms}" "${idle_ms}" "${queue_ms}" "${tasks}"
  done
done

echo "[3/3] done, csv=${CSV_OUT}"
