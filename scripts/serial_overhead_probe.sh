#!/usr/bin/env bash
# Isolate WHERE the small-b serial-path penalty comes from.
#
# Round 9 measured the whole 150-case suite with COMPILER2026_ASYNC_MIN_B=999, so
# every case took the Pass's serial path and no task was ever submitted. That run
# still came out at geomean 0.9916x overall and 0.93x on the b<12 bucket, i.e.
# 22 cases pay roughly 7% before any scheduling happens. Two candidate causes:
#
#   1. the build recipe -- block_cholesky.cpp goes through
#      `clang -emit-llvm -c` -> `opt` -> `clang++ <bc>`, so its IR is optimized
#      at -O2 twice, while the serial reference is compiled once, directly.
#   2. the Pass itself -- cloneForAsync() leaves two copies of the loop nest in
#      the module and insertAsyncDispatch() splits the entry block behind an
#      opaque call to compiler2026_runtime_should_async.
#
# Cause 1 would be an artifact of how we measure and would not exist on the
# judge, which integrates the Pass into one compiler invocation. Cause 2 would be
# a real cost we own. This probe separates them with a third binary that takes
# the same round trip through `opt` but runs NO pass.
#
# Only the 22 b<=10 cases are measured (4.8s of serial work), so the whole probe
# is cheap enough to iterate on.
#
# Usage (on the benchmark host, with the toolchain env loaded):
#   source ~/llvm17.env && ./scripts/serial_overhead_probe.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SUBMISSION_DIR="${REPO_ROOT}/submission"
SDK_DIR="${REPO_ROOT}/contestant_sdk"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/submission_bisheng}"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/build/serial_probe}"

CXX_BIN="${CXX:-/opt/bisheng/bin/clang++}"
CLANG_BIN="${CLANG:-/opt/bisheng/bin/clang++}"
OPT_BIN="${OPT:-/opt/bisheng/bin/opt}"

PASSES="${PASSES:-3}"
REPEAT="${REPEAT:-3}"
MAX_B="${MAX_B:-10}"
TASKSET_CPUS="${TASKSET_CPUS:-0-39}"
if [[ -n "${TASKSET_CPUS}" ]] && command -v taskset >/dev/null 2>&1; then
  RUNNER=(taskset -c "${TASKSET_CPUS}")
else
  RUNNER=()
fi

HARNESS_MAIN="${REPO_ROOT}/tools/percase_harness/main_percase.cpp"
BLOCK_CHOLESKY="${SUBMISSION_DIR}/src/baseline/block_cholesky.cpp"

"${SUBMISSION_DIR}/scripts/build.sh" >/dev/null

mkdir -p "${WORK_DIR}/bin" "${WORK_DIR}/ir"
cd "${SDK_DIR}"

COMMON_CXX=(-std=c++17 -O2 -pthread -Iinclude -Isrc/base_kernels)
KERNELS=(src/base_kernels/kernels_public.cpp src/base_kernels/kernels_impl.cpp)

echo "[1/5] building tools"
"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_generator/spd_generator.cpp -o "${WORK_DIR}/bin/spd_generator"
"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_verifier/verifier.cpp -o "${WORK_DIR}/bin/verifier"

echo "[2/5] building three variants of the same program"
# A: pristine -- one clang++ invocation, exactly the serial reference.
"${CXX_BIN}" "${COMMON_CXX[@]}" "${KERNELS[@]}" "${HARNESS_MAIN}" \
  "${BLOCK_CHOLESKY}" -o "${WORK_DIR}/bin/pristine"

# B: round-trip control -- same .bc detour as the contestant build, no pass.
"${CLANG_BIN}" -std=c++17 -O2 -Iinclude -Isrc/base_kernels \
  -emit-llvm -c "${BLOCK_CHOLESKY}" -o "${WORK_DIR}/ir/plain.bc"
"${OPT_BIN}" -passes=no-op-module "${WORK_DIR}/ir/plain.bc" \
  -o "${WORK_DIR}/ir/plain.opt.bc"
"${CXX_BIN}" "${COMMON_CXX[@]}" "${WORK_DIR}/ir/plain.opt.bc" \
  "${KERNELS[@]}" "${HARNESS_MAIN}" -o "${WORK_DIR}/bin/roundtrip"

# C: the real contestant build. Run with ASYNC_MIN_B=999 so every case takes the
# serial path and the only difference from B is the Pass transformation.
"${OPT_BIN}" -load-pass-plugin "${BUILD_DIR}/pass/libcontestant_pass.so" \
  -passes=contestant-pass "${WORK_DIR}/ir/plain.bc" \
  -o "${WORK_DIR}/ir/pass.opt.bc"
"${CXX_BIN}" "${COMMON_CXX[@]}" "${WORK_DIR}/ir/pass.opt.bc" \
  "${KERNELS[@]}" "${HARNESS_MAIN}" \
  "${BUILD_DIR}/runtime/libcontestant_runtime.a" -o "${WORK_DIR}/bin/pass"

echo "[3/5] generating input (b<=${MAX_B} cases)"
awk -F: -v maxb="${MAX_B}" '($2 + 0) <= maxb' \
  cases/preliminary_public_150.txt > "${WORK_DIR}/spec.txt"
echo "      $(wc -l < "${WORK_DIR}/spec.txt") cases"
if [[ ! -f "${WORK_DIR}/input.bin" ]] || \
   ! cmp -s "${WORK_DIR}/spec.txt" "${WORK_DIR}/spec.used"; then
  SPD_GENERATOR_THREADS="${GENERATOR_THREADS:-40}" \
    "${WORK_DIR}/bin/spd_generator" "${WORK_DIR}/input.bin" \
    --spec "${WORK_DIR}/spec.txt" >/dev/null
  cp "${WORK_DIR}/spec.txt" "${WORK_DIR}/spec.used"
else
  echo "      reusing ${WORK_DIR}/input.bin"
fi

echo "[4/5] measuring (passes=${PASSES}, in-process repeat=${REPEAT})"
# Every variant runs the serial path here, so no worker pool is ever built and
# in-process repeats cost nothing in fidelity -- they only cut noise.
#
# The variants are interleaved WITHIN each pass rather than run one after another.
# A first version ran all of pristine, then all of roundtrip, then all of pass,
# and the pristine/roundtrip ratio -- which is ~1.00 by construction -- came out
# at 1.0014x in one invocation and 1.0208x in the next. That 2% is host drift, and
# grouping by variant lets drift masquerade as a variant effect.
for pass_index in $(seq 1 "${PASSES}"); do
  for variant in pristine roundtrip pass; do
    env_prefix=()
    if [[ "${variant}" == "pass" ]]; then
      env_prefix=(env COMPILER2026_ASYNC_MIN_B=999)
    fi
    COMPILER2026_PERCASE_CSV="${WORK_DIR}/${variant}_p${pass_index}.csv" \
    COMPILER2026_PERCASE_REPEAT="${REPEAT}" \
      "${env_prefix[@]}" "${RUNNER[@]}" "${WORK_DIR}/bin/${variant}" \
      "${WORK_DIR}/input.bin" "${WORK_DIR}/${variant}.out" >/dev/null
  done
  echo "      pass ${pass_index}/${PASSES} done"
done
for variant in pristine roundtrip pass; do
  csvs=()
  for pass_index in $(seq 1 "${PASSES}"); do
    csvs+=("${WORK_DIR}/${variant}_p${pass_index}.csv")
  done
  python3 "${SCRIPT_DIR}/merge_percase.py" \
    --output "${WORK_DIR}/${variant}.csv" "${csvs[@]}" >/dev/null
done

echo "[5/5] verifying"
for variant in pristine roundtrip pass; do
  VERIFIER_THREADS="${VERIFIER_THREADS:-40}" \
    "${WORK_DIR}/bin/verifier" "${WORK_DIR}/input.bin" \
    "${WORK_DIR}/${variant}.out" > "${WORK_DIR}/${variant}.verify"
  pass_count=$(grep -c "status=PASS" "${WORK_DIR}/${variant}.verify" || true)
  total=$(grep -c "^case=" "${WORK_DIR}/${variant}.verify" || true)
  echo "      ${variant}: ${pass_count}/${total} PASS"
  if [[ "${pass_count}" -ne "${total}" || "${total}" -eq 0 ]]; then
    echo "serial_overhead_probe: verifier failures in ${variant}" >&2
    exit 4
  fi
done

echo
python3 - "${WORK_DIR}/pristine.csv" "${WORK_DIR}/roundtrip.csv" "${WORK_DIR}/pass.csv" <<'PY'
import csv, math, sys

def load(path):
    out = {}
    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            out[int(row["case_index"])] = {
                "n": int(row["n"]), "b": int(row["b"]),
                "s": float(row["seconds_min"]),
            }
    return out

pristine, roundtrip, passed = (load(p) for p in sys.argv[1:4])
geo = lambda v: math.exp(sum(math.log(x) for x in v) / len(v))

print("Ratios are reference/variant, so <1.00 means the variant is SLOWER.")
print("pass/roundtrip is the one that isolates the Pass: both sides took the")
print("same .bc detour, so it cancels the build recipe out.")
print()
print("   idx     n   b    B  pristine_s   rt/pris   pass/pris   pass/rt")
rt_ratios, pass_ratios, norm_ratios = [], [], []
for index in sorted(pristine):
    a, b_, c = pristine[index], roundtrip[index], passed[index]
    rt = a["s"] / b_["s"]
    pr = a["s"] / c["s"]
    norm = b_["s"] / c["s"]
    rt_ratios.append(rt)
    pass_ratios.append(pr)
    norm_ratios.append(norm)
    print(f"  {index:4d} {a['n']:5d} {a['b']:3d} {a['n'] // a['b']:4d} "
          f"{a['s']:11.6f} {rt:8.3f}x {pr:10.3f}x {norm:8.3f}x")
print()
print(f"  geomean over {len(rt_ratios)} cases:"
      f"   rt/pris={geo(rt_ratios):.4f}x"
      f"   pass/pris={geo(pass_ratios):.4f}x"
      f"   pass/rt={geo(norm_ratios):.4f}x")
print()
print("  rt/pris is ~1.00 by construction -- treat its distance from 1.00 as this")
print("  invocation's noise floor, and only believe a pass/rt move that clears it.")
print()
by_b = {}
for index in sorted(pristine):
    by_b.setdefault(pristine[index]["b"], []).append(
        roundtrip[index]["s"] / passed[index]["s"])
for b_value in sorted(by_b):
    group = by_b[b_value]
    print(f"    b={b_value:3d}: {len(group):2d} cases  pass/rt={geo(group):.4f}x")
PY
