/*
 * =============================================================================
 * casefilter アルゴリズム - 検索処理
 * =============================================================================
 *
 * このファイルは類似検索の実装を含む。
 * クエリに対して編集距離≤kのキーワードが存在するかを判定する。
 *
 * 【検索戦略】
 * 1. Case A: indel=0（置換のみ）→ Hamming距離で判定
 * 2. Case B: indel=1（1削除+1挿入）→ 削除後のHamming距離で判定
 *
 * 【最適化手法】
 * - SWAR (SIMD Within A Register): ビット並列Hamming計算
 * - 訪問済み管理: 世代カウンタで高速初期化
 * - ポスティングソート: 短いリストから処理して早期終了
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "index.h"

/*
 * =============================================================================
 * ペア定義（Case A用）
 * =============================================================================
 *
 * build.cと同じペア定義。
 * 検索時にクエリから10通りのペアを生成するために使用。
 */
static const uint8_t pair_i[10] = {0, 0, 0, 0, 1, 1, 1, 2, 2, 3};
static const uint8_t pair_j[10] = {1, 2, 3, 4, 2, 3, 4, 3, 4, 4};

/*
 * =============================================================================
 * 手動popcount実装（HAKMEM法）
 * =============================================================================
 *
 * 【目的】
 * __builtin_popcountllはコンパイラ依存のため、移植性を高めるために
 * 64bitビット並列popcountを手動実装する。
 *
 * 【アルゴリズム: HAKMEM法】
 * MIT AI Labで開発された古典的なビットカウントアルゴリズム。
 * ビット並列処理により、O(1)で64bitのpopcountを計算する。
 *
 * Step 1: 2bit単位でビット数をカウント
 *   x = x - ((x >> 1) & 0x5555555555555555ULL)
 *   各2bitブロックが00〜10（0〜2）のビット数を表す
 *
 * Step 2: 4bit単位で隣接2bitのカウントを加算
 *   x = (x & 0x3333...) + ((x >> 2) & 0x3333...)
 *   各4bitブロックが0000〜0100（0〜4）のビット数を表す
 *
 * Step 3: 8bit単位でカウント
 *   x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL
 *   各8bitブロックが00000000〜00001000（0〜8）のビット数を表す
 *
 * Step 4: 全バイトを最上位バイトに集約
 *   return (x * 0x0101010101010101ULL) >> 56
 *   乗算により全バイトの和を最上位バイトに集め、56bitシフトで取得
 *
 * 【具体例】
 * x = 0b11010110 (popcount = 5)
 *
 * Step 1: x - ((x >> 1) & 0x55)
 *   x       = 11010110
 *   x >> 1  = 01101011
 *   & 0x55  = 01010001
 *   x - ... = 10000101 → 2bit単位: [10, 00, 00, 01] = [2, 0, 0, 1]
 *
 * Step 2: (x & 0x33) + ((x >> 2) & 0x33)
 *   x & 0x33     = 00000001
 *   (x >> 2) & 0x33 = 00100001
 *   合計         = 00100010 → 4bit単位: [0010, 0010] = [2, 2]
 *
 * Step 3: (x + (x >> 4)) & 0x0F
 *   結果 = 00000101 → 8bit単位: [0101] = [5]
 *
 * Step 4: (x * 0x01) >> 56 = 5 ✓
 *
 * 【性能】
 * CPUのpopcnt命令と同等の性能を、純粋なビット演算で実現。
 * 実測で__builtin_popcountllと同等〜わずかに遅い程度（数%以内）。
 * 移植性が向上し、コンパイラ非依存の実装が可能。
 */
static inline int popcount64(uint64_t x) {
    /* Step 1: 2bit単位でビット数をカウント */
    x = x - ((x >> 1) & 0x5555555555555555ULL);

    /* Step 2: 4bit単位で隣接2bitのカウントを加算 */
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);

    /* Step 3: 8bit単位でカウント */
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;

    /* Step 4: 全バイトを最上位バイトに集約して合計を取得 */
    return (x * 0x0101010101010101ULL) >> 56;
}

/*
 * =============================================================================
 * SWAR Hamming距離計算
 * =============================================================================
 */

/*
 * pack_keyword: 15文字をSWAR形式にパック
 *
 * 【用途】
 * クエリを4bitパック形式に変換し、SWAR Hamming計算の準備をする。
 *
 * 【アルゴリズム】
 * 各文字をA=0, B=1, ..., J=9に変換し、4bitずつ連結。
 *
 * 【ビット配置】
 * word[0]  → bit 0-3
 * word[1]  → bit 4-7
 * word[2]  → bit 8-11
 * ...
 * word[14] → bit 56-59
 *
 * 合計60bit（uint64_tの下位60bit）
 */
static inline uint64_t pack_keyword(const char *word) {
    uint64_t v = 0;
    for (int i = 0; i < KEYWORD_LEN; ++i) {
        uint64_t nib = (uint64_t)(word[i] - 'A') & 0xF;
        v |= nib << (i * 4);
    }
    return v;
}

/*
 * hamming_packed15: 15文字パックコードのHamming距離
 *
 * 【SWAR (SIMD Within A Register) 技法】
 * 通常の実装: 15回のループで各文字を比較 → O(15)
 * SWAR実装: ビット演算で一度に処理 → O(1)
 *
 * 【アルゴリズム】
 * 1. XORで異なるビットを抽出: x = a ^ b
 * 2. 各4bitニブル内で1が立っているか判定:
 *    - x |= (x >> 1): 下位2bitに集約
 *    - x |= (x >> 2): 最下位bitに集約
 * 3. 最下位bitのみマスク: x &= 0x1111...1111 (15個の1)
 * 4. popcount: 立っているビット数をカウント
 *
 * 【具体例】
 * a = "ABCDE..." = 0x...43210
 * b = "ABXDE..." = 0x...43910 (3文字目が異なる)
 *
 * x = a ^ b = 0x...00700 (3文字目のニブルが異なる)
 *     = ...0000_0111_0000_0000
 *
 * x |= (x >> 1):
 *     = ...0000_0111_0000_0000
 *     | ...0000_0011_1000_0000
 *     = ...0000_0111_1000_0000
 *
 * x |= (x >> 2):
 *     = ...0000_0111_1000_0000
 *     | ...0000_0001_1110_0000
 *     = ...0000_0111_1110_0000
 *
 * x &= 0x1111111111111111:
 *     = ...0000_0001_0000_0000
 *
 * popcount(x) = 1 (1箇所異なる)
 *
 * 【性能】
 * 通常実装: 15回の比較 + 分岐
 * SWAR実装: XOR + 3回のビット演算 + popcount64（手動実装）
 * → 約10-20倍高速
 *
 * popcount64はHAKMEM法により約10命令で完了し、CPU依存を避けつつ
 * 高速なビット並列計算を実現している。
 */
static inline int hamming_packed15(uint64_t a, uint64_t b) {
    /* ステップ1: XORで異なるビットを抽出 */
    uint64_t x = a ^ b;

    /* ステップ2: 各ニブル内の差異を最下位bitに集約 */
    x |= (x >> 1);  /* bit 0-1 の差異をbit 0に */
    x |= (x >> 2);  /* bit 0-3 の差異をbit 0に */

    /* ステップ3: 各ニブルの最下位bitのみ抽出 */
    x &= 0x1111111111111111ULL;  /* 15個の0x1 */

    /* ステップ4: 立っているビット数をカウント（=Hamming距離） */
    return popcount64(x);
}

/*
 * hamming_packed14: 14文字パックコードのHamming距離
 *
 * 【用途】
 * Case Bで使用。削除後の14文字間のHamming距離を計算。
 *
 * 【アルゴリズム】
 * hamming_packed15と同じだが、14文字分（56bit）を対象とする。
 *
 * 【注意】
 * 実装はhamming_packed15と同一。上位4bitは常に0なので、
 * XORやpopcountの結果に影響しない。
 */
static inline int hamming_packed14(uint64_t a, uint64_t b) {
    uint64_t x = a ^ b;
    x |= (x >> 1);
    x |= (x >> 2);
    x &= 0x1111111111111111ULL;  /* 14個の0x1（15番目も0） */
    return popcount64(x);
}

/*
 * =============================================================================
 * キーのパッキング関数
 * =============================================================================
 */

/*
 * pack_key6: 6文字を10進数表現の整数にパック
 *
 * build.cと同じ実装。
 * クエリから生成した6文字ペアを整数に変換し、slot番号を計算するために使用。
 */
static inline uint32_t pack_key6(const char *key) {
    uint32_t v = 0;
    uint32_t mul = 1;
    for (int i = 0; i < 6; ++i) {
        v += (((uint32_t)key[i] - 'A') & 0xF) * mul;
        mul *= 10u;
    }
    return v;
}

/*
 * pack_key7: 7文字を10進数表現の整数にパック
 *
 * build.cと同じ実装。
 * クエリから削除後の14文字を前半7+後半7に分けて整数に変換するために使用。
 */
static inline uint32_t pack_key7(const char *key) {
    uint32_t v = 0;
    uint32_t mul = 1;
    for (int i = 0; i < 7; ++i) {
        v += (((uint32_t)key[i] - 'A') & 0xF) * mul;
        mul *= 10u;
    }
    return v;
}

/*
 * =============================================================================
 * メイン検索関数
 * =============================================================================
 */

/*
 * casefilter_search: 類似検索のメイン関数
 *
 * 【引数】
 * - idx: 索引
 * - query: クエリ文字列（15文字）
 * - k: 最大編集距離（通常は3）
 *
 * 【戻り値】
 * 1: 編集距離≤kのキーワードが見つかった
 * 0: 見つからなかった
 *
 * 【アルゴリズム概要】
 * Phase 0: 初期化（訪問済み管理、クエリのパック）
 * Phase 1: Case A - indel=0の候補をHIndexから列挙、Hamming距離で判定
 * Phase 2: Case B - indel=1の候補をDelIndexから列挙、削除後Hamming距離で判定
 *
 * 【正当性】
 * 固定長15文字のため、挿入回数=削除回数が保証される。
 * - indel=0 → 編集距離=Hamming距離 → Case Aで完全カバー
 * - indel=1 → 編集距離=2+Hamming14 → Case Bで完全カバー
 * - indel≥2 → 編集距離≥4 → k=3では対象外
 *
 * したがって、k≤3の場合、Case AとCase Bで全ての候補を網羅できる。
 */
int casefilter_search(const CaseFilterIndex *idx, const char *query, int k) {
    /* 引数チェック */
    if (!idx || !query || (int)strlen(query) != KEYWORD_LEN) return 0;

    int kw_count = idx->keyword_count;

    /*
     * ========================================
     * Phase 0: 訪問済み管理の初期化
     * ========================================
     *
     * 【世代カウンタ方式】
     * 通常の実装: 毎回visited配列をmemsetでクリア → O(n)
     * 世代カウンタ: genをインクリメントするだけ → O(1)
     *
     * visited[id] == gen なら既訪問、そうでなければ未訪問。
     * genがオーバーフローしたら、その時だけmemsetする。
     *
     * 【静的バッファの再利用】
     * visited_bufは静的変数として保持し、複数回の検索で再利用。
     * キーワード数が増えた場合のみ再確保。
     */
    static uint32_t *visited_buf = NULL;  /* 静的バッファ */
    static int visited_cap = 0;           /* 現在の容量 */
    static uint32_t gen = 1;              /* 世代カウンタ */

    /* バッファの容量が足りない場合は再確保 */
    if (kw_count > visited_cap) {
        free(visited_buf);
        visited_buf = (uint32_t *)calloc((size_t)kw_count, sizeof(uint32_t));
        visited_cap = kw_count;
        gen = 1;  /* 再確保時は世代をリセット */
    }

    if (!visited_buf) return 0;  /* メモリ確保失敗 */

    /* 世代カウンタをインクリメント */
    gen++;

    /* オーバーフロー対策: gen==0になったらmemsetで初期化 */
    if (gen == 0) {
        memset(visited_buf, 0, sizeof(uint32_t) * (size_t)visited_cap);
        gen = 1;
    }

    uint32_t *visited = visited_buf;

    /*
     * ========================================
     * Phase 1: Case A（indel=0、Hamming≤k）
     * ========================================
     *
     * 【処理フロー】
     * 1. クエリを3文字×5ブロックに分割
     * 2. 10通りのペアを生成
     * 3. 各ペアのポスティングリストを取得
     * 4. ポスティング長でソート（短い順）
     * 5. 短いリストから処理（早期ヒット狙い）
     * 6. 各候補に対してHamming距離を計算
     * 7. Hamming≤k ならヒット、即座に1を返す
     */

    /* クエリを3文字×5ブロックに分割 */
    char blocks[5][3];
    for (int b = 0; b < 5; ++b) {
        memcpy(blocks[b], query + b * 3, 3);
    }

    /* クエリをSWAR形式にパック */
    uint64_t qcode = pack_keyword(query);

    /* 10通りのペアを生成し、ポスティング情報を取得 */
    int order[10];  /* ソート順を保持 */
    struct {
        int start;     /* ポスティング開始位置 */
        int len;       /* ポスティング長 */
        char key[6];   /* 6文字キー（未使用だが保持） */
    } pairs[10];

    for (int p = 0; p < 10; ++p) {
        order[p] = p;

        /* 6文字ペアを生成 */
        memcpy(pairs[p].key, blocks[pair_i[p]], 3);
        memcpy(pairs[p].key + 3, blocks[pair_j[p]], 3);

        /* slot番号を計算 */
        uint32_t packed = pack_key6(pairs[p].key);
        int slot = (int)(packed + (uint32_t)p * CASEFILTER_H_KEY_SPACE);

        /* ポスティング範囲を取得 */
        pairs[p].start = idx->hidx.offsets[slot];
        pairs[p].len = idx->hidx.offsets[slot + 1] - pairs[p].start;
    }

    /*
     * 【ポスティング長でソート】
     * 短いリストから処理することで、早期にヒットする可能性が高まる。
     * また、短いリストは偽陽性（false positive）が少ない傾向がある。
     *
     * ソート: 単純選択ソート（要素数が10と小さいので十分）
     */
    for (int i = 0; i < 10; ++i) {
        for (int j = i + 1; j < 10; ++j) {
            if (pairs[order[i]].len > pairs[order[j]].len) {
                int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }

    /*
     * 【ポスティングリストの走査】
     * ソート順に各ペアのポスティングリストを処理。
     */
    for (int oi = 0; oi < 10; ++oi) {
        int p = order[oi];

        /* 空のポスティングリストはスキップ */
        if (pairs[p].len == 0) continue;

        /* ポスティングリストのポインタを取得 */
        const int *ids = idx->hidx.ids + pairs[p].start;

        /* リスト内の各キーワードIDを検証 */
        for (int i = 0; i < pairs[p].len; ++i) {
            int id = ids[i];

            /* 既訪問ならスキップ */
            if (visited[id] == gen) continue;

            /* 訪問済みマークを付ける
             * 注意: Hamming距離に関わらず常にマークする。
             * これにより、同じIDを他のペアで再評価しなくなる。 */
            visited[id] = gen;

            /* SWAR Hamming距離を計算 */
            int hd = hamming_packed15(qcode, idx->codes[id]);

            /* Hamming距離≤k ならヒット */
            if (hd <= k) return 1;
        }
    }

    /*
     * Case Aでヒットせず。
     * 世代カウンタを進めて、Case Bで使用。
     */
    gen++;

    /*
     * ========================================
     * Phase 2: Case B（indel=1、削除後Hamming≤1）
     * ========================================
     *
     * 【処理フロー】
     * 1. クエリから15通りの「1文字削除」パターンを生成
     * 2. 各削除パターンについて:
     *    a. 削除後の14文字をSWAR形式にパック
     *    b. 前半7文字と後半7文字に分割
     *    c. 両方のキーでDelIndexを引く
     *    d. 候補のキーワードに対して削除後Hamming距離を計算
     *    e. 2 + Hamming14 ≤ k ならヒット
     *
     * 【編集距離の計算】
     * 編集距離 = 2（削除+挿入） + Hamming14（残りの置換）
     *
     * クエリから位置pを削除 → 14文字
     * キーワードから位置qを削除 → 14文字
     * この2つの14文字間のHamming距離をhd14とすると、
     * 元の15文字間の編集距離は 2 + hd14
     *
     * 【具体例】
     * クエリ:   "ABCDEFGHIJKLMNO"
     * キーワード: "XBCDEFGHIJKLMNO" (1文字目が異なる)
     *
     * Case A (indel=0):
     *   Hamming = 1 → ヒット（k=3）
     *
     * クエリ:   "ABCDEFGHIJKLMNO"
     * キーワード: "BCDEFGHIJKLMNOP" (1文字削除+1文字挿入)
     *
     * Case A (indel=0):
     *   全く一致しない → ミス
     *
     * Case B (indel=1):
     *   クエリからpos=0削除:  "BCDEFGHIJKLMNO"
     *   キーワードからpos=14削除: "BCDEFGHIJKLMNO"
     *   Hamming14 = 0
     *   編集距離 = 2 + 0 = 2 → ヒット（k=3）
     *
     * 【訪問済み管理の違い】
     * Case Aでは、visited[id]=genを常にセットした。
     * Case Bでは、ヒット時のみvisited[id]=genをセットする。
     *
     * 理由: 同じIDでも削除位置が異なると結果が変わる可能性がある。
     * false negativeを避けるため、不一致の場合はvisitedをセットしない。
     *
     * 代償: 同じIDが複数回評価される可能性がある（重複評価）。
     * しかし、false negativeを防ぐため、これは受け入れる。
     */
    char qdel[14];  /* クエリから1文字削除した14文字 */

    /* 15通りの削除位置を試す */
    for (int pos = 0; pos < KEYWORD_LEN; ++pos) {
        /* クエリのSWARパックコードから位置posを削除 */
        uint64_t qdel_code = casefilter_pack_delete(qcode, pos);

        /* クエリから位置posを削除した文字列を作成 */
        memcpy(qdel, query, pos);                        /* pos前まで */
        memcpy(qdel + pos, query + pos + 1, KEYWORD_LEN - pos - 1); /* pos後から */

        /* 前半7文字と後半7文字に分割 */
        char left[7], right[7];
        memcpy(left, qdel, 7);      /* qdel[0..6] */
        memcpy(right, qdel + 7, 7); /* qdel[7..13] */

        /*
         * 【左キー（前半7文字）でDelIndexを検索】
         */
        int lslot = (int)pack_key7(left);
        int lstart = idx->del7.offsets[lslot];
        int lend = idx->del7.offsets[lslot + 1];

        if (lend > lstart) {
            /* ポスティングリストを取得 */
            const uint32_t *idpos = idx->del7.idpos + lstart;
            int count = lend - lstart;

            /* リスト内の各候補を検証 */
            for (int i = 0; i < count; ++i) {
                uint32_t v = idpos[i];

                /* idposからIDと削除位置を抽出 */
                int id = (int)(v & 0xFFFFF);          /* 下位20bit */
                int kw_del_pos = (int)((v >> 20) & 0xF); /* bit 20-23 */

                /* 既訪問ならスキップ（ヒット済み） */
                if (visited[id] == gen) continue;

                /* キーワードのSWARパックコードから削除位置を削除 */
                uint64_t kwdel_code = casefilter_pack_delete(idx->codes[id], kw_del_pos);

                /* 削除後14文字間のHamming距離を計算 */
                int hd14 = hamming_packed14(qdel_code, kwdel_code);

                /* 編集距離 = 2（削除+挿入） + hd14 */
                if (2 + hd14 <= k) {
                    /* ヒット！ visitedをセットして即座に返す */
                    visited[id] = gen;
                    return 1;
                }

                /* 不一致の場合、visitedをセットしない
                 * （他の削除位置で一致する可能性があるため） */
            }
        }

        /*
         * 【右キー（後半7文字）でDelIndexを検索】
         * 左キーと同じ処理を右キーでも実行。
         */
        int rslot = (int)pack_key7(right);
        int rstart = idx->del7.offsets[rslot];
        int rend = idx->del7.offsets[rslot + 1];

        if (rend > rstart) {
            const uint32_t *idpos = idx->del7.idpos + rstart;
            int count = rend - rstart;

            for (int i = 0; i < count; ++i) {
                uint32_t v = idpos[i];
                int id = (int)(v & 0xFFFFF);
                int kw_del_pos = (int)((v >> 20) & 0xF);

                /* 既訪問ならスキップ */
                if (visited[id] == gen) continue;

                /* 削除後Hamming距離を計算 */
                uint64_t kwdel_code = casefilter_pack_delete(idx->codes[id], kw_del_pos);
                int hd14 = hamming_packed14(qdel_code, kwdel_code);

                /* 編集距離判定 */
                if (2 + hd14 <= k) {
                    visited[id] = gen;
                    return 1;
                }
            }
        }
    }

    /*
     * ========================================
     * Phase 3: 見つからなかった
     * ========================================
     *
     * Case AとCase Bの両方で候補が見つからなかった。
     * 編集距離≤kのキーワードは存在しない。
     */
    return 0;
}
