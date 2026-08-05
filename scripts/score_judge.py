#!/usr/bin/env python3
"""Compute the judge's scoring metric from two per-case timing CSVs.

The judge scores an EQUAL-WEIGHT geometric mean of per-case speedups:

    speedup_i         = T0_i / T_i                     (per case)
    geo_speedup       = (prod speedup_i) ** (1/N)
    performance_score = 100 * geo_speedup / m_ideal
    total_score       = 0.4 * functional_score + 0.6 * performance_score

`m_ideal` and the weights come from `docs/technical_scheme_notes.md`; the default
`m_ideal=32.0` is what the judge log reported (`m_ideal_value_source=builtin:32.0`)
for the 2026-08-05 run that scored 44.38 at geo_speedup 2.338006.

Inputs are the CSVs written by `tools/percase_harness/main_percase.cpp`.

Usage:
    scripts/score_judge.py serial.csv contestant.csv [--m-ideal 32] \
        [--stat min|median|first] [--merged-csv out.csv] [--top 20]
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from collections import defaultdict


def read_percase(path: str, stat: str) -> dict[int, dict]:
    column = {"min": "seconds_min", "median": "seconds_median", "first": "seconds_first"}[stat]
    rows: dict[int, dict] = {}
    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            index = int(row["case_index"])
            rows[index] = {
                "n": int(row["n"]),
                "b": int(row["b"]),
                "seconds": float(row[column]),
            }
    if not rows:
        raise SystemExit(f"{path}: no rows")
    return rows


def geomean(values: list[float]) -> float:
    return math.exp(sum(math.log(v) for v in values) / len(values))


def b_bucket(b: int) -> str:
    if b < 12:
        return "b<12 (serial path)"
    if b < 32:
        return "12<=b<32"
    if b < 128:
        return "32<=b<128"
    return "b>=128"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("serial_csv")
    parser.add_argument("contestant_csv")
    parser.add_argument("--m-ideal", type=float, default=32.0)
    parser.add_argument("--stat", default="min", choices=["min", "median", "first"])
    parser.add_argument("--functional", type=float, default=100.0)
    parser.add_argument("--merged-csv")
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--label", default="")
    args = parser.parse_args()

    serial = read_percase(args.serial_csv, args.stat)
    contestant = read_percase(args.contestant_csv, args.stat)

    missing = set(serial) ^ set(contestant)
    if missing:
        raise SystemExit(f"case index mismatch between the two CSVs: {sorted(missing)[:10]}")

    cases = []
    for index in sorted(serial):
        s, c = serial[index], contestant[index]
        if (s["n"], s["b"]) != (c["n"], c["b"]):
            raise SystemExit(f"case {index}: (n,b) mismatch {s} vs {c}")
        if c["seconds"] <= 0.0:
            raise SystemExit(f"case {index}: non-positive contestant time")
        cases.append(
            {
                "case_index": index,
                "n": s["n"],
                "b": s["b"],
                "blocks": s["n"] // s["b"],
                "serial_seconds": s["seconds"],
                "contestant_seconds": c["seconds"],
                "speedup": s["seconds"] / c["seconds"],
            }
        )

    speedups = [c["speedup"] for c in cases]
    geo = geomean(speedups)
    perf = 100.0 * geo / args.m_ideal
    total = 0.4 * args.functional + 0.6 * perf
    total_serial = sum(c["serial_seconds"] for c in cases)
    total_contestant = sum(c["contestant_seconds"] for c in cases)

    if args.label:
        print(f"label={args.label}")
    print(f"cases={len(cases)} stat={args.stat} m_ideal={args.m_ideal}")
    print(f"geometric_mean_speedup={geo:.6f}")
    print(f"total_time_ratio={total_serial / total_contestant:.6f}   "
          f"(flops-weighted; what benchmark.sh reports -- NOT the judge metric)")
    print(f"performance_score={perf:.2f}")
    print(f"total_score={total:.2f}   (functional={args.functional:.2f})")
    print(f"score_per_extra_1x_geomean={0.6 * 100.0 / args.m_ideal:.3f}")
    print()

    # Where the geometric mean is lost. Every case contributes log(speedup)/N to
    # log(geo), so a case at 1.0x contributes exactly 0 -- these are the cases
    # that hold the metric down, regardless of how small they are.
    print(f"--- per-case speedup distribution ---")
    for lo, hi in [(0, 0.95), (0.95, 1.05), (1.05, 2), (2, 4), (4, 8), (8, 16), (16, 1e9)]:
        group = [c for c in cases if lo <= c["speedup"] < hi]
        if group:
            label = f"[{lo:g}, {hi:g})" if hi < 1e9 else f"[{lo:g}, inf)"
            print(f"  {label:>14s}: {len(group):3d} cases  geo={geomean([g['speedup'] for g in group]):6.2f}x")
    print()

    print("--- by block size bucket (equal weight, so case COUNT is what matters) ---")
    buckets: dict[str, list[dict]] = defaultdict(list)
    for c in cases:
        buckets[b_bucket(c["b"])].append(c)
    order = ["b<12 (serial path)", "12<=b<32", "32<=b<128", "b>=128"]
    for name in order:
        group = buckets.get(name)
        if not group:
            continue
        g = geomean([c["speedup"] for c in group])
        # Score if this bucket alone were lifted to the overall geomean of the rest.
        rest = [c["speedup"] for c in cases if b_bucket(c["b"]) != name]
        lifted = geomean(rest + [geomean(rest)] * len(group)) if rest else g
        print(f"  {name:20s}: {len(group):3d} cases  geo={g:6.2f}x  "
              f"share_of_log={sum(math.log(c['speedup']) for c in group) / sum(math.log(c['speedup']) for c in cases) * 100:5.1f}%  "
              f"score_if_lifted_to_rest={0.4 * args.functional + 0.6 * 100 * lifted / args.m_ideal:5.2f}")
    print()

    print(f"--- {args.top} worst cases by speedup (each one costs "
          f"{0.6 * 100 / args.m_ideal / len(cases):.4f} points per 1% of speedup) ---")
    print("   idx     n     b   B   serial_s  contest_s  speedup")
    for c in sorted(cases, key=lambda c: c["speedup"])[: args.top]:
        print(f"  {c['case_index']:4d} {c['n']:5d} {c['b']:5d} {c['blocks']:3d} "
              f"{c['serial_seconds']:10.6f} {c['contestant_seconds']:10.6f} {c['speedup']:7.3f}x")
    print()

    # What geomean is needed for a target score, and what the remaining cases
    # would have to average if the sub-1.05x cases stayed where they are.
    for target in (50.0, 55.0, 60.0):
        need_geo = (target - 0.4 * args.functional) / (0.6 * 100.0 / args.m_ideal)
        print(f"score {target:.0f} needs geomean {need_geo:.2f}x "
              f"({need_geo / geo:.2f}x above current)")

    if args.merged_csv:
        with open(args.merged_csv, "w", newline="") as handle:
            writer = csv.DictWriter(
                handle,
                fieldnames=["case_index", "n", "b", "blocks", "serial_seconds",
                            "contestant_seconds", "speedup"],
            )
            writer.writeheader()
            for c in cases:
                writer.writerow(c)
        print(f"\nmerged_csv={args.merged_csv}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
