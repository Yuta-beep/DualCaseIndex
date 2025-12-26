# ケース分解フィルタ方式（indel=0/1 → Hamming化）

## 目的
編集距離≤3（長さ15固定・文字種A–J）の存在判定を、Levenshtein DPを回さずに「Hamming問題＋分割ハッシュ」で高速化する厳密手法。

## コアアイデア
- 長さ固定なので挿入=削除。距離≤3なら **indelは0か1のみ**。
- **indel=0:** 置換のみ → Hamming距離≤3。
- **indel=1:** 1削除+1挿入を含む。両者から適切な位置を1文字削除すると長さ14同士で Hamming距離≤1 に帰着。

## 索引構成
### A) indel=0（Hamming≤3）
- 文字列を **3/3/3/3/3** に5分割。
- Hamming≤3なら「5ブロックのうち少なくとも2ブロック一致」が必要。
- 2ブロック連結（6文字）の **10通りのペアキー**を倒置索引化。

### B) indel=1（14文字Hamming≤1）
- 各文字列の「1文字削除」14文字列を15通り生成。
- 14文字を **7/7** に分割し、左7キー→右7リスト と右7キー→左7リストの2系統を保持。

## 検索手順
1. **Case A（indel=0）**  
   - クエリを5分割し、10ペアキーを生成。バケット長が短い順に走査。  
   - 候補に対して `hamming15` で≤3なら `levenshtein_banded(max=3)` で確定判定。ヒットで即終了。
2. **Case B（indel=1）**
   - クエリを削除位置0–14で14文字列化。左7/右7キーで索引引き。
   - `hamming14` ≤1なら `levenshtein_banded(max=3)` で確定判定。ヒットで即終了。
3. 見つからなければ0。

## ファイル
- `index.h` : 構造体定義とシグネチャ。
- `build.c` : 索引構築・シリアライズ/デシリアライズ。
- `search.c`: 検索（ケース分解＋Hamming→Levenshtein）。
- `src/main/prep_casefilter.c`, `src/main/search_casefilter.c`: 専用エントリポイント。

## ビルド
```bash
gcc -O2 -lm src/alg/casefilter/build.c src/main/prep_casefilter.c -o prep_casefilter
gcc -O2 -lm src/alg/casefilter/build.c src/alg/casefilter/search.c src/main/search_casefilter.c -o search_casefilter
```

## 実行例
```bash
./prep_casefilter test-data/db_1 > output/index_casefilter
./search_casefilter test-data/query_1 output/index_casefilter > output/result_casefilter
```

## 実行時間を記録する例
```bash
/usr/bin/time -f 'search %e' ./search_casefilter test-data/query_100k output/index_casefilter_100k > /dev/null
# record_perf でCSV記録
gcc -O2 src/common/record_perf.c -o record_perf  # 未ビルドなら
./record_perf --record -- ./search_casefilter test-data/query_100k output/index_casefilter_100k > /dev/null
```

## 今後の改善ポイント
- ポスティングをCSR＋bit-pack（IDは20bit目安）で圧縮し、200MB制約に合わせる。
- バケット長統計を持ち、探索順序の最適化や早期終了を強化。
- `fread`戻り値チェックで警告除去。
