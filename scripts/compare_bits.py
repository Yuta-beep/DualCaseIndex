#!/usr/bin/env python3
"""
Compare two bitstring result files (0/1) via Hamming distance.

Example:
  python3 scripts/compare_bits.py --pred output/result_casefilter_500k \
      --truth ground-truth/result_baseline_500k
"""
import argparse
import sys


def load_bits(path: str) -> str:
    """Load a file and keep only 0/1 characters."""
    try:
        data = open(path, "r", encoding="utf-8").read()
    except OSError as e:
        sys.exit(f"failed to read {path}: {e}")
    bits = "".join(ch for ch in data if ch in "01")
    if not bits:
        sys.exit(f"{path}: no 0/1 data found")
    return bits


def main() -> None:
    parser = argparse.ArgumentParser(description="Compare bitstring results with Hamming distance")
    parser.add_argument("--pred", required=True, help="predicted result file (0/1 bitstring)")
    parser.add_argument("--truth", required=True, help="ground truth result file (0/1 bitstring)")
    parser.add_argument(
        "--max-show",
        type=int,
        default=20,
        help="show up to this many mismatch samples (default: 20)",
    )
    args = parser.parse_args()

    pred = load_bits(args.pred)
    truth = load_bits(args.truth)

    min_len = min(len(pred), len(truth))
    max_len = max(len(pred), len(truth))

    mismatches = []
    for i in range(min_len):
        if pred[i] != truth[i]:
            mismatches.append((i, truth[i], pred[i]))
    # Extra tail contributes to distance
    tail_diff = abs(len(pred) - len(truth))
    hamming = len(mismatches) + tail_diff

    accuracy = 0.0
    if max_len > 0:
        accuracy = 100.0 * (1.0 - hamming / max_len)

    print(f"truth length      : {len(truth)}")
    print(f"pred length       : {len(pred)}")
    print(f"compared positions: {min_len}")
    print(f"hamming distance  : {hamming}")
    print(f"accuracy          : {accuracy:.6f}%")

    if tail_diff > 0:
        print(f"length mismatch   : +{tail_diff} treated as differences")

    if mismatches and args.max_show > 0:
        print(f"first {min(len(mismatches), args.max_show)} mismatches (pos, truth, pred):")
        for pos, t, p in mismatches[: args.max_show]:
            print(f"  {pos}: {t} vs {p}")


if __name__ == "__main__":
    main()
