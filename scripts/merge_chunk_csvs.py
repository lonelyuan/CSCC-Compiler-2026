#!/usr/bin/env python3
"""Combine per-chunk merged CSVs from percase_bench_chunked.sh into two
harness-format CSVs so scripts/score_judge.py can score the union.

Each chunk's merged CSV (written by score_judge.py --merged-csv) numbers its
cases 0..k-1 relative to that chunk's spec range, so the chunk's spec start is
added back to recover the global case index. A duplicate global index means two
chunks overlapped, which would silently double-weight cases in the geometric
mean, so it is a hard error.

Usage:
    merge_chunk_csvs.py --serial-out s.csv --contestant-out c.csv \
        chunk1.csv:1 chunk2.csv:104 ...
"""

from __future__ import annotations

import argparse
import csv
import sys

HARNESS_FIELDS = [
    "case_index", "n", "b", "repeats",
    "seconds_min", "seconds_median", "seconds_max", "seconds_first",
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("chunks", nargs="+", metavar="CSV:SPEC_START")
    parser.add_argument("--serial-out", required=True)
    parser.add_argument("--contestant-out", required=True)
    args = parser.parse_args()

    serial: dict[int, dict] = {}
    contestant: dict[int, dict] = {}

    for spec in args.chunks:
        path, _, start_text = spec.rpartition(":")
        if not path:
            raise SystemExit(f"expected CSV:SPEC_START, got {spec!r}")
        offset = int(start_text) - 1
        with open(path, newline="") as handle:
            rows = list(csv.DictReader(handle))
        if not rows:
            raise SystemExit(f"{path}: no rows")
        for row in rows:
            index = int(row["case_index"]) + offset
            if index in serial:
                raise SystemExit(
                    f"case {index} appears in more than one chunk "
                    f"(second occurrence in {path}) -- chunk ranges overlap"
                )
            common = {"case_index": index, "n": int(row["n"]), "b": int(row["b"]), "repeats": 1}
            for target, column in ((serial, "serial_seconds"), (contestant, "contestant_seconds")):
                seconds = float(row[column])
                target[index] = dict(
                    common,
                    seconds_min=seconds,
                    seconds_median=seconds,
                    seconds_max=seconds,
                    seconds_first=seconds,
                )

    for path, table in ((args.serial_out, serial), (args.contestant_out, contestant)):
        with open(path, "w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=HARNESS_FIELDS)
            writer.writeheader()
            for index in sorted(table):
                writer.writerow(table[index])

    print(f"merged {len(serial)} cases from {len(args.chunks)} chunks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
