#!/usr/bin/env python3
"""Merge several per-case timing CSVs into one, taking the per-case minimum.

Each input CSV comes from one whole-process pass of
`tools/percase_harness/main_percase.cpp`. Taking the minimum across passes
suppresses turbo/thermal noise while preserving the judge's structure: within a
pass every case is still measured on its first call, so per-case runtime setup
(worker-pool construction when the resolved thread count changes) is included.
"""

from __future__ import annotations

import argparse
import csv
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("inputs", nargs="+")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    merged: dict[int, dict] = {}
    for path in args.inputs:
        with open(path, newline="") as handle:
            for row in csv.DictReader(handle):
                index = int(row["case_index"])
                current = {
                    "case_index": index,
                    "n": int(row["n"]),
                    "b": int(row["b"]),
                    "repeats": int(row["repeats"]),
                    "seconds_min": float(row["seconds_min"]),
                    "seconds_median": float(row["seconds_median"]),
                    "seconds_max": float(row["seconds_max"]),
                    "seconds_first": float(row["seconds_first"]),
                }
                previous = merged.get(index)
                if previous is None:
                    merged[index] = current
                    continue
                if (previous["n"], previous["b"]) != (current["n"], current["b"]):
                    raise SystemExit(f"case {index}: (n,b) mismatch across passes")
                previous["repeats"] += current["repeats"]
                previous["seconds_min"] = min(previous["seconds_min"], current["seconds_min"])
                previous["seconds_max"] = max(previous["seconds_max"], current["seconds_max"])
                # Keep the best (lowest) first-call time across passes: still a
                # first call, just the least noisy one observed.
                previous["seconds_first"] = min(previous["seconds_first"], current["seconds_first"])
                previous["seconds_median"] = min(previous["seconds_median"],
                                                 current["seconds_median"])

    if not merged:
        raise SystemExit("no rows merged")

    with open(args.output, "w", newline="") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=["case_index", "n", "b", "repeats", "seconds_min", "seconds_median",
                        "seconds_max", "seconds_first"],
        )
        writer.writeheader()
        for index in sorted(merged):
            writer.writerow(merged[index])
    print(f"merged {len(args.inputs)} passes -> {args.output} ({len(merged)} cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
