#!/usr/bin/env bash
# Sanity test for record_perf wrapper.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

if [[ ! -x ./record_perf ]]; then
  echo "record_perf binary not found. Build with: gcc -O2 src/common/record_perf.c -o record_perf" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

OUT_FILE="$TMP_DIR/out.txt"
CSV_DIR="$TMP_DIR/records"

./record_perf --record --records-dir "$CSV_DIR" -- ./src/test/fake_search.sh dummy_query dummy_index > "$OUT_FILE"

# Output should match fake_search (three lines: 1,0,1)
diff <(printf "1\n0\n1\n") "$OUT_FILE"

CSV_FILE="$CSV_DIR/perf_dummy_query.csv"
test -f "$CSV_FILE"

# CSV should contain header and a single data row with hit_count=2 and return_code=0
grep -q "^timestamp_utc,executable,query_file,index_file,dataset,elapsed_seconds,hit_count,return_code$" "$CSV_FILE"
grep -q ",./src/test/fake_search.sh,dummy_query,dummy_index,dummy_query,.*,2,0$" "$CSV_FILE" || {
  echo "CSV content did not match expectations:" >&2
  cat "$CSV_FILE" >&2
  exit 1
}

echo "record_perf test passed"
