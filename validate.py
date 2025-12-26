#!/usr/bin/env python3
"""
索引構造に依存しない検証スクリプト。
- 任意の prep/search バイナリを指定し、与えた DB/Query に対して
  1) 索引構築（prep）
  2) 探索（search）
  3) ナイーブ照合（Pythonで距離<=k）
を行い、出力ビット列の一致を確認する。

使い方例:
    python3 src/tests/validate.py \
        --prep-bin ./prep_dev \
        --search-bin ./search_dev \
        --db test-data/db_1 \
        --query test-data/query_1

既に生成済みの索引を使う場合:
    python3 src/tests/validate.py --search-bin ./search_dev \
        --db test-data/db_1 --query test-data/query_1 --index output/index_bk
"""

import argparse
import subprocess
import tempfile
import os
import sys
from typing import List

KEYWORD_LEN = 15


def levenshtein_banded(a: str, b: str, maxd: int) -> int:
    """固定長15、帯域付きレーベンシュタイン（早期打ち切り付き）。"""
    len1 = len(a)
    len2 = len(b)
    dp = list(range(len2 + 1))
    for i in range(1, len1 + 1):
        prev = dp[0]
        dp[0] = i
        j_start = max(1, i - maxd)
        j_end = min(len2, i + maxd)
        row_min = dp[0]
        for j in range(j_start, j_end + 1):
            tmp = dp[j]
            cost = 0 if a[i - 1] == b[j - 1] else 1
            v = min(dp[j] + 1, dp[j - 1] + 1, prev + cost)
            dp[j] = v
            prev = tmp
            if v < row_min:
                row_min = v
        if row_min > maxd:
            return maxd + 1
    return dp[len2]


def naive_expect(db: List[str], queries: List[str], maxd: int) -> List[int]:
    res = []
    for q in queries:
        hit = 0
        for w in db:
            if levenshtein_banded(q, w, maxd) <= maxd:
                hit = 1
                break
        res.append(hit)
    return res


def read_words(path: str, limit: int = None) -> List[str]:
    out = []
    with open(path, "r") as f:
        for line in f:
            s = line.strip()
            if not s:
                continue
            out.append(s)
            if limit and len(out) >= limit:
                break
    return out


def parse_bits(output: str) -> List[int]:
    bits = []
    for line in output.strip().splitlines():
        line = line.strip()
        if not line:
            continue
        for ch in line:
            if ch in ("0", "1"):
                bits.append(int(ch))
            else:
                raise ValueError(f"Unexpected character in output: {ch!r}")
    return bits


def run_cmd(cmd: List[str], stdout_path: str = None):
    if stdout_path:
        with open(stdout_path, "wb") as out:
            subprocess.run(cmd, check=True, stdout=out)
    else:
        return subprocess.run(cmd, check=True, capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prep-bin", help="索引構築バイナリのパス")
    ap.add_argument("--search-bin", required=True, help="探索バイナリのパス")
    ap.add_argument("--db", required=True, help="DBファイルパス")
    ap.add_argument("--query", required=True, help="クエリファイルパス")
    ap.add_argument("--index", help="既存索引ファイルパス（指定時はprepをスキップ）")
    ap.add_argument("--maxdist", type=int, default=3, help="距離閾値(デフォルト3)")
    ap.add_argument("--limit-db", type=int, help="DBの最大読み込み件数（サンプル検証用）")
    ap.add_argument("--limit-query", type=int, help="クエリの最大読み込み件数（サンプル検証用）")
    args = ap.parse_args()

    if not args.index and not args.prep_bin:
        ap.error("--index か --prep-bin のどちらかが必要です")

    if args.index and (args.limit_db or args.limit_query):
        ap.error("--index 使用時は limit を指定しないでください（不一致の元になります）")

    if (args.limit_db is None) != (args.limit_query is None):
        ap.error("サンプル検証をする場合は --limit-db と --limit-query を両方指定してください")

    # サンプル用の一時ファイルを作る（limit指定時）
    sample_db = args.db
    sample_query = args.query
    temp_files = []
    if args.limit_db is not None and args.limit_query is not None:
        db_words = read_words(args.db, args.limit_db)
        queries = read_words(args.query, args.limit_query)
        print(f"[load sample] db={len(db_words)} queries={len(queries)}")
        # 書き出し
        fd_db, sample_db = tempfile.mkstemp(prefix="db_sample_", suffix=".txt")
        fd_q, sample_query = tempfile.mkstemp(prefix="query_sample_", suffix=".txt")
        with os.fdopen(fd_db, "w") as f:
            for w in db_words:
                f.write(w + "\n")
        with os.fdopen(fd_q, "w") as f:
            for q in queries:
                f.write(q + "\n")
        temp_files.extend([sample_db, sample_query])
    else:
        db_words = read_words(args.db)  # フルロード（大規模なら注意）
        queries = read_words(args.query)
        print(f"[load full] db={len(db_words)} queries={len(queries)}")

    temp_index = None
    index_path = args.index
    if index_path is None:
        fd, index_path = tempfile.mkstemp(prefix="index_", suffix=".bin")
        os.close(fd)
        temp_index = index_path
        print(f"[prep] {args.prep_bin} {sample_db} > {index_path}")
        run_cmd([args.prep_bin, sample_db], stdout_path=index_path)

    print(f"[search] {args.search_bin} {sample_query} {index_path}")
    search_out = run_cmd([args.search_bin, sample_query, index_path])
    got_bits = parse_bits(search_out.stdout)

    expect_bits = naive_expect(db_words, queries, args.maxdist)

    mismatches = [(i, e, g) for i, (e, g) in enumerate(zip(expect_bits, got_bits)) if e != g]
    status = "OK" if not mismatches and len(expect_bits) == len(got_bits) else "NG"

    print(f"[result] expect={len(expect_bits)} got={len(got_bits)} mismatches={len(mismatches)} => {status}")
    if mismatches:
        print(" first mismatches (up to 10):")
        for i, e, g in mismatches[:10]:
            print(f"  idx {i}: expect {e} got {g}")
        sys.exit(1)
    if len(expect_bits) != len(got_bits):
        print(" length mismatch between expect and got")
        sys.exit(1)

    for p in temp_files:
        try:
            os.remove(p)
        except OSError:
            pass
    if temp_index:
        try:
            os.remove(temp_index)
        except OSError:
            pass


if __name__ == "__main__":
    main()
