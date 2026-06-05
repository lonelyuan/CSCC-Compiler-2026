#!/usr/bin/env python3
import argparse
from pathlib import Path


CASE_GROUPS = [
    (128, [8, 16, 32, 64, 128]),
    (192, [8, 16, 24, 32, 48, 64, 96]),
    (256, [8, 16, 32, 64, 128, 256]),
    (320, [8, 10, 16, 20, 32, 40, 64]),
    (384, [8, 12, 16, 24, 32, 48, 64, 96, 128]),
    (448, [8, 14, 16, 28, 32, 56, 64, 112]),
    (512, [8, 16, 32, 64, 128, 256]),
    (576, [8, 9, 12, 16, 18, 24, 32, 36, 48, 64, 72, 96, 144]),
    (640, [8, 10, 16, 20, 32, 40, 64, 80, 128]),
    (768, [8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256]),
    (896, [8, 14, 16, 28, 32, 56, 64, 112, 128]),
    (1024, [8, 16, 32, 64, 128, 256]),
    (1152, [8, 9, 12, 16, 18, 24, 32, 36, 48, 64, 72, 96, 128, 144, 192, 288]),
    (1280, [8, 10, 16, 20, 32, 40, 64, 80, 128, 160, 256]),
    (1536, [8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384]),
    (1792, [8, 14, 16, 28, 32, 56, 64, 112, 128, 224, 256]),
    (2048, [8, 16, 32, 64])
]


def emit_cases():
    rows = []
    pair_index = 0
    for n, block_sizes in CASE_GROUPS:
        for b in block_sizes:
            if n % b != 0:
                raise ValueError(f"Invalid pair: n={n}, b={b}")
            seed = pair_index * 100 + 1
            rows.append(f"{n}:{b}:{seed}")
            pair_index += 1
    return rows


def main():
    parser = argparse.ArgumentParser(
        description="Generate the public 150-case preliminary spec file."
    )
    parser.add_argument("output", help="Output spec file path")
    args = parser.parse_args()

    rows = emit_cases()
    if len(rows) != 150:
        raise ValueError(f"Expected 150 cases, got {len(rows)}")

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(rows) + "\n", encoding="utf-8")
    print(f"Wrote {len(rows)} cases to {output}")


if __name__ == "__main__":
    main()
