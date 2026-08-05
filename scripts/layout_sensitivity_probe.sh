#!/usr/bin/env bash
# How much of the b=8 "Pass penalty" is just where the linker put the operators?
#
# The serial-path probe found that the 17 b=8 cases run ~10% slower with the Pass
# applied than without it, even though every one of them takes the serial path and
# the serial body is code-equivalent. Two structural fixes (keeping the async clone
# outlined, then outlining the serial body too so block_cholesky is nothing but the
# dispatch) did not move that number. Meanwhile __official_madd_impl -- a 275-byte
# function that a b=8 case calls on the order of n^3/6b^3 times -- sits at a
# different 64-byte offset in the two binaries (%64=32 vs %64=48).
#
# This probe builds the SAME pass-free program several times, changing nothing but
# the number of padding bytes linked ahead of the kernels, which shifts every later
# text address. Every variant is functionally and instruction-wise identical, so
# whatever spread appears is pure code placement. If that spread is on the order of
# the 10% we were chasing, then the "penalty" is a property of this host's linker
# layout rather than of the Pass, and it should not be optimized against.
#
# Usage (on the benchmark host, with the toolchain env loaded):
#   source ~/llvm17.env && ./scripts/layout_sensitivity_probe.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SUBMISSION_DIR="${REPO_ROOT}/submission"
SDK_DIR="${REPO_ROOT}/contestant_sdk"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/build/layout_probe}"

CXX_BIN="${CXX:-/opt/bisheng/bin/clang++}"
PASSES="${PASSES:-3}"
REPEAT="${REPEAT:-3}"
# 0 is the unpadded reference. The others are deliberately not multiples of 64 so
# they land the kernels on different cache-line and fetch-window offsets.
PAD_BYTES="${PAD_BYTES:-0 16 32 48 80}"
TASKSET_CPUS="${TASKSET_CPUS:-0-39}"
if [[ -n "${TASKSET_CPUS}" ]] && command -v taskset >/dev/null 2>&1; then
  RUNNER=(taskset -c "${TASKSET_CPUS}")
else
  RUNNER=()
fi

HARNESS_MAIN="${REPO_ROOT}/tools/percase_harness/main_percase.cpp"
BLOCK_CHOLESKY="${SUBMISSION_DIR}/src/baseline/block_cholesky.cpp"

mkdir -p "${WORK_DIR}/bin"
cd "${SDK_DIR}"

COMMON_CXX=(-std=c++17 -O2 -pthread -Iinclude -Isrc/base_kernels)
KERNELS=(src/base_kernels/kernels_public.cpp src/base_kernels/kernels_impl.cpp)

echo "[1/4] building tools"
"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_generator/spd_generator.cpp -o "${WORK_DIR}/bin/spd_generator"
"${CXX_BIN}" -std=c++17 -O2 -pthread -Iinclude \
  tools/local_verifier/verifier.cpp -o "${WORK_DIR}/bin/verifier"

echo "[2/4] building one binary per padding size"
variants=()
for pad in ${PAD_BYTES}; do
  variant="pad${pad}"
  variants+=("${variant}")
  # A padding object placed first on the link line. It is never called; it exists
  # only to push the kernels' text forward by `pad` bytes.
  cat > "${WORK_DIR}/pad.cpp" <<EOF
extern "C" void compiler2026_layout_pad() {
    __asm__ volatile(".space ${pad}, 0x90");
}
EOF
  if [[ "${pad}" -eq 0 ]]; then
    "${CXX_BIN}" "${COMMON_CXX[@]}" "${KERNELS[@]}" "${HARNESS_MAIN}" \
      "${BLOCK_CHOLESKY}" -o "${WORK_DIR}/bin/${variant}"
  else
    "${CXX_BIN}" "${COMMON_CXX[@]}" -c "${WORK_DIR}/pad.cpp" \
      -o "${WORK_DIR}/pad.o"
    "${CXX_BIN}" "${COMMON_CXX[@]}" "${WORK_DIR}/pad.o" "${KERNELS[@]}" \
      "${HARNESS_MAIN}" "${BLOCK_CHOLESKY}" -o "${WORK_DIR}/bin/${variant}"
  fi
  # No early `exit` in awk and no `head`: either would close the pipe while nm is
  # still writing, and under `set -o pipefail` that SIGPIPE fails the script.
  align=$(nm --print-size "${WORK_DIR}/bin/${variant}" \
    | awk '/__official_madd_impl/ { a = strtonum("0x" $1) % 64 } END { print a }')
  echo "      ${variant}: __official_madd_impl addr%64=${align}"
done

echo "[3/4] generating input (b=8 cases only)"
awk -F: '($2 + 0) == 8' cases/preliminary_public_150.txt > "${WORK_DIR}/spec.txt"
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

echo "[4/4] measuring (passes=${PASSES}, in-process repeat=${REPEAT})"
for pass_index in $(seq 1 "${PASSES}"); do
  for variant in "${variants[@]}"; do
    COMPILER2026_PERCASE_CSV="${WORK_DIR}/${variant}_p${pass_index}.csv" \
    COMPILER2026_PERCASE_REPEAT="${REPEAT}" \
      "${RUNNER[@]}" "${WORK_DIR}/bin/${variant}" \
      "${WORK_DIR}/input.bin" "${WORK_DIR}/${variant}.out" >/dev/null
  done
  echo "      pass ${pass_index}/${PASSES} done"
done
for variant in "${variants[@]}"; do
  csvs=()
  for pass_index in $(seq 1 "${PASSES}"); do
    csvs+=("${WORK_DIR}/${variant}_p${pass_index}.csv")
  done
  python3 "${SCRIPT_DIR}/merge_percase.py" \
    --output "${WORK_DIR}/${variant}.csv" "${csvs[@]}" >/dev/null
  VERIFIER_THREADS="${VERIFIER_THREADS:-40}" \
    "${WORK_DIR}/bin/verifier" "${WORK_DIR}/input.bin" \
    "${WORK_DIR}/${variant}.out" > "${WORK_DIR}/${variant}.verify"
  ok=$(grep -c "status=PASS" "${WORK_DIR}/${variant}.verify" || true)
  total=$(grep -c "^case=" "${WORK_DIR}/${variant}.verify" || true)
  if [[ "${ok}" -ne "${total}" || "${total}" -eq 0 ]]; then
    echo "layout_sensitivity_probe: verifier failures in ${variant}" >&2
    exit 4
  fi
done
echo "      all variants ${total}/${total} PASS"

echo
python3 - "${WORK_DIR}" "${variants[@]}" <<'PY'
import csv, math, os, sys

work, names = sys.argv[1], sys.argv[2:]

def load(name):
    out = {}
    path = os.path.join(work, f"{name}.csv")
    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            out[int(row["case_index"])] = {
                "n": int(row["n"]), "b": int(row["b"]),
                "s": float(row["seconds_min"]),
            }
    return out

data = {name: load(name) for name in names}
geo = lambda v: math.exp(sum(math.log(x) for x in v) / len(v))
ref = names[0]

print("Every binary here is the SAME pass-free program; only the number of padding")
print("bytes linked ahead of the kernels differs. Ratios are pad0/variant.")
print()
header = "   idx     n    B" + "".join(f"{name:>10s}" for name in names[1:])
print(header)
for index in sorted(data[ref]):
    row = data[ref][index]
    cells = "".join(f"{row['s'] / data[name][index]['s']:9.3f}x" for name in names[1:])
    print(f"  {index:4d} {row['n']:5d} {row['n'] // row['b']:4d}{cells}")
print()
print("  geomean" + " " * 13 + "".join(
    f"{geo([data[ref][i]['s'] / data[name][i]['s'] for i in data[ref]]):9.4f}x"
    for name in names[1:]))
print()
per_case_spread = []
for index in sorted(data[ref]):
    times = [data[name][index]["s"] for name in names]
    per_case_spread.append(max(times) / min(times))
overall = [geo([data[name][i]["s"] for i in data[ref]]) for name in names]
print(f"  worst per-case spread across layouts: {max(per_case_spread):.3f}x")
print(f"  median per-case spread:               "
      f"{sorted(per_case_spread)[len(per_case_spread) // 2]:.3f}x")
print(f"  spread of the whole-suite geomean:    {max(overall) / min(overall):.3f}x")
print()
print("  If these spreads are comparable to the ~10% attributed to the Pass, then")
print("  code placement dominates at b=8 and that 10% is not a Pass property.")
PY
