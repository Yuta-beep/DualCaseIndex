/*
 * =============================================================================
 * casefilter アルゴリズム - 索引構築とシリアライズ
 * =============================================================================
 *
 * このファイルは以下の処理を実装：
 * 1. キーワードの挿入と容量管理
 * 2. HIndex（Case A用）の構築
 * 3. DelIndex（Case B用）の構築
 * 4. 索引のシリアライズ/デシリアライズ
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "index.h"
#include "../../common/types.h"

/* 定数のエイリアス（可読性向上） */
#define HPAIR_COUNT CASEFILTER_HPAIR_COUNT
#define H_KEY_SPACE CASEFILTER_H_KEY_SPACE
#define DEL_KEY_SPACE CASEFILTER_DEL_KEY_SPACE

/*
 * =============================================================================
 * ペア定義（Case A用）
 * =============================================================================
 *
 * 15文字を3文字×5ブロックに分割した時の、10通りのペアを定義。
 * ブロック番号: 0, 1, 2, 3, 4
 * ペア: (0,1), (0,2), (0,3), (0,4), (1,2), (1,3), (1,4), (2,3), (2,4), (3,4)
 *
 * 【鳩の巣原理】
 * Hamming距離≤3の場合、5ブロック中少なくとも2ブロックは一致する。
 * したがって、全ペアC(5,2)=10を索引化すれば漏れがない。
 *
 * 【具体例】
 * キーワード: "ABCDEFGHIJKLMNO"
 * ブロック0: "ABC", ブロック1: "DEF", ブロック2: "GHI", ブロック3: "JKL", ブロック4: "MNO"
 *
 * pair 0 (i=0, j=1): "ABCDEF" (ブロック0+ブロック1)
 * pair 1 (i=0, j=2): "ABCGHI" (ブロック0+ブロック2)
 * ...
 * pair 9 (i=3, j=4): "JKLMNO" (ブロック3+ブロック4)
 */
static const uint8_t pair_i[HPAIR_COUNT] = {0, 0, 0, 0, 1, 1, 1, 2, 2, 3};
static const uint8_t pair_j[HPAIR_COUNT] = {1, 2, 3, 4, 2, 3, 4, 3, 4, 4};

/*
 * =============================================================================
 * キーのパッキング関数
 * =============================================================================
 */

/*
 * pack_key6: 6文字を10進数表現の整数にパック
 *
 * 【アルゴリズム】
 * 各文字をA=0, B=1, ..., J=9に変換し、10進数として解釈。
 * 例: "ABCDEF" → 543210 (10進数)
 *
 * 【実装の詳細】
 * LSB側から順に各文字を配置：
 * - key[0]の文字を1の位
 * - key[1]の文字を10の位
 * - key[2]の文字を100の位
 * - ...
 *
 * 【具体例】
 * key = "ABCDEF" = ['A', 'B', 'C', 'D', 'E', 'F']
 * i=0: v = 0 + (0 & 0xF) * 1 = 0
 * i=1: v = 0 + (1 & 0xF) * 10 = 10
 * i=2: v = 10 + (2 & 0xF) * 100 = 210
 * i=3: v = 210 + (3 & 0xF) * 1000 = 3210
 * i=4: v = 3210 + (4 & 0xF) * 10000 = 43210
 * i=5: v = 43210 + (5 & 0xF) * 100000 = 543210
 * 結果: 543210
 *
 * 【注意】
 * & 0xFは下位4bitのマスク。文字コード変換後の余計なビットを除去。
 */
static inline uint32_t pack_key6(const char *key) {
    uint32_t v = 0;
    uint32_t mul = 1;  /* 10の累乗（1, 10, 100, ...） */
    for (int i = 0; i < 6; ++i) {
        v += (((uint32_t)key[i] - 'A') & 0xF) * mul;
        mul *= 10u;
    }
    return v;
}

/*
 * pack_key7: 7文字を10進数表現の整数にパック
 *
 * 【アルゴリズム】
 * pack_key6と同様だが、7文字を対象とする。
 * 例: "ABCDEFG" → 6543210 (10進数)
 *
 * 【値の範囲】
 * 最小: "AAAAAAA" = 0
 * 最大: "JJJJJJJ" = 9999999
 * → 7桁の10進数（0〜9,999,999）
 */
static inline uint32_t pack_key7(const char *key) {
    uint32_t v = 0;
    uint32_t mul = 1;  /* 10の累乗（1, 10, 100, ...） */
    for (int i = 0; i < 7; ++i) {
        v += (((uint32_t)key[i] - 'A') & 0xF) * mul;
        mul *= 10u;
    }
    return v;
}

/*
 * =============================================================================
 * ファイルI/O補助関数
 * =============================================================================
 */

/*
 * fread_exact: 正確なサイズを読み込む（エラーチェック付き）
 *
 * 【用途】
 * デシリアライズ時のエラー処理を簡潔にする。
 * fread()の戻り値をチェックし、期待通りの要素数が読めたかを確認。
 *
 * 【戻り値】
 * 1: 成功（nバイト読めた）
 * 0: 失敗（nバイト読めなかった）
 */
static int fread_exact(void *dst, size_t size, size_t n, FILE *in) {
    return fread(dst, size, n, in) == n;
}

/*
 * =============================================================================
 * 索引の作成と挿入
 * =============================================================================
 */

/*
 * ensure_keyword_cap: キーワード配列の容量を確保
 *
 * 【動作】
 * keyword_count が keyword_cap に達したら、配列を2倍に拡張。
 * 動的に拡張することで、事前にサイズを知る必要がなくなる。
 *
 * 【拡張戦略】
 * - 初回: 0 → 1024
 * - 以降: 現在の2倍（倍々ゲーム）
 *
 * 【対象配列】
 * - keywords: 生キーワード配列
 * - codes: SWAR用パック配列
 */
static void ensure_keyword_cap(CaseFilterIndex *idx) {
    if (idx->keyword_count < idx->keyword_cap) return;  /* 容量に余裕あり */

    /* 新しい容量を計算（2倍、または初回は1024） */
    int new_cap = idx->keyword_cap ? idx->keyword_cap * 2 : 1024;

    /* キーワード配列を拡張 */
    idx->keywords = (char (*)[KEYWORD_LEN + 1])realloc(
        idx->keywords,
        sizeof(char[KEYWORD_LEN + 1]) * new_cap
    );

    /* パック配列を拡張 */
    idx->codes = (uint64_t *)realloc(
        idx->codes,
        sizeof(uint64_t) * new_cap
    );

    idx->keyword_cap = new_cap;
}

/*
 * casefilter_create: 索引の作成
 *
 * 【引数】
 * capacity: 初期容量（0以下の場合は1024）
 *
 * 【初期化内容】
 * - CaseFilterIndex構造体をゼロ初期化（calloc）
 * - キーワード配列とパック配列を確保
 * - HIndexとDelIndexは未初期化（casefilter_finalizeで構築）
 */
CaseFilterIndex *casefilter_create(int capacity) {
    /* 構造体をゼロ初期化 */
    CaseFilterIndex *idx = (CaseFilterIndex *)calloc(1, sizeof(CaseFilterIndex));

    /* 初期容量を設定 */
    idx->keyword_cap = capacity > 0 ? capacity : 1024;

    /* キーワード配列を確保 */
    idx->keywords = (char (*)[KEYWORD_LEN + 1])malloc(
        sizeof(char[KEYWORD_LEN + 1]) * idx->keyword_cap
    );

    /* パック配列を確保 */
    idx->codes = (uint64_t *)malloc(sizeof(uint64_t) * idx->keyword_cap);

    return idx;
}

/*
 * casefilter_insert: キーワードの挿入
 *
 * 【処理フロー】
 * 1. 容量チェック＆拡張（必要なら）
 * 2. 生キーワードをコピー
 * 3. SWAR用にパック（4bit×15文字=60bit）
 * 4. keyword_countをインクリメント
 *
 * 【パッキングの詳細】
 * 各文字を4bitにパックし、1つのuint64_tに格納：
 *
 * word = "ABCDEFGHIJKLMNO"
 *        [0][1][2][3][4][5][6][7][8][9][10][11][12][13][14]
 *
 * code = [14][13][12][11][10][9][8][7][6][5][4][3][2][1][0]
 *        ↑                                              ↑
 *        MSB側                                        LSB側
 *
 * 各文字は4bit（0-9）で表現：
 * - 'A' → 0 (0000)
 * - 'B' → 1 (0001)
 * - ...
 * - 'J' → 9 (1001)
 *
 * 【具体例】
 * word = "ABCDEFGHIJKLMNO"
 * code = 0x0E_D_C_B_A_9_8_7_6_5_4_3_2_1_0
 *        ↑15個の4bitニブル↑
 */
void casefilter_insert(CaseFilterIndex *idx, const char *word) {
    if (!idx || !word) return;

    /* 容量を確保 */
    ensure_keyword_cap(idx);

    /* 生キーワードをコピー（15文字+終端） */
    memcpy(idx->keywords[idx->keyword_count], word, KEYWORD_LEN);
    idx->keywords[idx->keyword_count][KEYWORD_LEN] = '\0';

    /* SWAR用にパック: 各文字を4bitにして連結 */
    uint64_t code = 0;
    for (int i = 0; i < KEYWORD_LEN; ++i) {
        /* 文字をA=0, B=1, ..., J=9に変換 */
        uint64_t nib = (uint64_t)(word[i] - 'A') & 0xF;

        /* i番目の位置（i*4bit）に配置 */
        code |= nib << (i * 4);
    }
    idx->codes[idx->keyword_count] = code;

    idx->keyword_count++;
}

/*
 * =============================================================================
 * HIndex構築（Case A用）
 * =============================================================================
 */

/*
 * build_hindex: Case A用の索引を構築
 *
 * 【アルゴリズム概要】
 * 2パスアルゴリズムでCSR（Compressed Sparse Row）形式を構築：
 *
 * Pass 1: 各キーの出現頻度をカウント
 * Pass 2: 実際のIDを連結配列に格納
 *
 * 【データ構造】
 * HIndex = CSR形式の転置インデックス
 * - counts[slot]: 各slotのキーワード数
 * - offsets[slot]: 各slotのポスティング開始位置（prefix sum）
 * - ids[offset...offset+count-1]: 実際のキーワードID
 *
 * 【slot番号の計算】
 * 15文字を3文字×5ブロックに分割し、10通りのペアを生成：
 *
 * キーワード: "ABCDEFGHIJKLMNO"
 * ブロック: [ABC][DEF][GHI][JKL][MNO]
 *           [0]  [1]  [2]  [3]  [4]
 *
 * ペア0 (i=0, j=1): "ABCDEF" → pack_key6("ABCDEF") + 0 * 1M
 * ペア1 (i=0, j=2): "ABCGHI" → pack_key6("ABCGHI") + 1 * 1M
 * ...
 * ペア9 (i=3, j=4): "JKLMNO" → pack_key6("JKLMNO") + 9 * 1M
 *
 * 【メモリレイアウト】
 * slots = 1,000,000 × 10 = 10,000,000
 *
 * counts[0...9,999,999]: 各slotのキーワード数
 * offsets[0...10,000,000]: prefix sum（最後は総数）
 * ids[0...total-1]: 連結されたキーワードID
 */
static void build_hindex(CaseFilterIndex *idx) {
    HIndex *h = &idx->hidx;
    h->key_space = H_KEY_SPACE;  /* 1,000,000 */
    h->pair_count = HPAIR_COUNT;  /* 10 */

    /* 総slot数を計算 */
    int slots = h->key_space * h->pair_count;  /* 10,000,000 */

    /* counts配列を確保＆ゼロ初期化 */
    h->counts = (uint32_t *)calloc((size_t)slots, sizeof(uint32_t));

    /*
     * ========================================
     * Pass 1: 各slotの出現頻度をカウント
     * ========================================
     */
    for (int id = 0; id < idx->keyword_count; ++id) {
        const char *w = idx->keywords[id];

        /* 15文字を3文字×5ブロックに分割 */
        char blocks[5][3];
        for (int b = 0; b < 5; ++b) {
            memcpy(blocks[b], w + b * 3, 3);
        }

        /* 10通りのペアを生成してカウント */
        for (int p = 0; p < HPAIR_COUNT; ++p) {
            /* ペアiとペアjを連結して6文字キーを作成 */
            char key[6];
            memcpy(key, blocks[pair_i[p]], 3);      /* 前半3文字 */
            memcpy(key + 3, blocks[pair_j[p]], 3);  /* 後半3文字 */

            /* slotを計算: pack_key6(key) + pair_id * 1M */
            int slot = (int)(pack_key6(key) + (uint32_t)p * H_KEY_SPACE);

            /* カウント */
            h->counts[slot]++;
        }
    }

    /*
     * ========================================
     * prefix sumでoffsetsを計算
     * ========================================
     *
     * counts = [3, 0, 4, 3, ...]
     * offsets = [0, 3, 3, 7, 10, ...]
     *          ↑     ↑     ↑     ↑
     *          slot0 slot1 slot2 slot3
     *
     * offsets[i+1] = offsets[i] + counts[i]
     */
    h->offsets = (int *)malloc(sizeof(int) * (size_t)(slots + 1));
    h->offsets[0] = 0;
    for (int i = 0; i < slots; ++i) {
        h->offsets[i + 1] = h->offsets[i] + (int)h->counts[i];
    }

    /* 総ID数を取得 */
    int total_ids = h->offsets[slots];

    /* ids配列を確保 */
    h->ids = (int *)malloc(sizeof(int) * (size_t)total_ids);

    /*
     * ========================================
     * Pass 2: 実際のIDを連結配列に格納
     * ========================================
     *
     * cursor配列: 各slotの次に書き込む位置を追跡
     * 初期値はoffsetsのコピー
     */
    int *cursor = (int *)malloc(sizeof(int) * (size_t)slots);
    memcpy(cursor, h->offsets, sizeof(int) * (size_t)slots);

    /* 再度全キーワードを走査 */
    for (int id = 0; id < idx->keyword_count; ++id) {
        const char *w = idx->keywords[id];

        /* ブロック分割 */
        char blocks[5][3];
        for (int b = 0; b < 5; ++b) {
            memcpy(blocks[b], w + b * 3, 3);
        }

        /* 10通りのペアに対してIDを登録 */
        for (int p = 0; p < HPAIR_COUNT; ++p) {
            char key[6];
            memcpy(key, blocks[pair_i[p]], 3);
            memcpy(key + 3, blocks[pair_j[p]], 3);

            int slot = (int)(pack_key6(key) + (uint32_t)p * H_KEY_SPACE);

            /* cursor位置にIDを書き込み、cursorを進める */
            h->ids[cursor[slot]++] = id;
        }
    }

    free(cursor);
}

/*
 * =============================================================================
 * DelIndex構築（Case B用）
 * =============================================================================
 */

/*
 * build_dindex: Case B用の索引を構築
 *
 * 【アルゴリズム概要】
 * HIndexと同様に2パスアルゴリズムでCSR形式を構築するが、以下が異なる：
 *
 * 1. 各キーワードから15通りの「1文字削除」パターンを生成
 * 2. 削除後の14文字を前半7文字+後半7文字に分割
 * 3. 左右の7文字キーを同じ索引（DelIndex）に登録
 * 4. IDと削除位置をパックして格納（id 20bit | del_pos 4bit）
 *
 * 【削除パターンの生成】
 * キーワード: "ABCDEFGHIJKLMNO" (15文字)
 *
 * pos=0を削除: "BCDEFGHIJKLMNO" (14文字)
 *              left="BCDEFGH", right="IJKLMNO"
 * pos=1を削除: "ACDEFGHIJKLMNO"
 *              left="ACDEFGH", right="IJKLMNO"
 * ...
 * pos=14を削除: "ABCDEFGHIJKLMN"
 *               left="ABCDEFG", right="HIJKLMN"
 *
 * 【なぜ左右に分けるか】
 * 14文字をそのままキーにすると、キー空間が10^14になり巨大。
 * 前半7+後半7に分けることで、キー空間を10^7×2に削減。
 * 検索時は両方のキーを引くことで候補を列挙する。
 *
 * 【idposのパッキング】
 * 32bit = [unused 8bit][del_pos 4bit][id 20bit]
 *
 * 例: id=123, del_pos=7
 *     idpos = (123 & 0xFFFFF) | (7 << 20)
 *           = 0x0070007B
 */
static void build_dindex(CaseFilterIndex *idx) {
    DelIndex *d = &idx->del7;
    d->key_space = DEL_KEY_SPACE;  /* 10,000,000 */

    /* counts配列を確保＆ゼロ初期化 */
    d->counts = (uint32_t *)calloc((size_t)d->key_space, sizeof(uint32_t));

    /*
     * ========================================
     * Pass 1: 各7文字キーの出現頻度をカウント
     * ========================================
     */
    char del[14];  /* 削除後の14文字を一時保存 */

    for (int id = 0; id < idx->keyword_count; ++id) {
        const char *w = idx->keywords[id];

        /* 15通りの削除位置を試す */
        for (int pos = 0; pos < KEYWORD_LEN; ++pos) {
            /* pos位置の文字を除いて14文字を作成 */
            for (int i = 0, k = 0; i < KEYWORD_LEN; ++i) {
                if (i == pos) continue;  /* pos位置をスキップ */
                del[k++] = w[i];
            }

            /* 前半7文字と後半7文字に分割 */
            char left[7], right[7];
            memcpy(left, del, 7);       /* del[0..6] */
            memcpy(right, del + 7, 7);  /* del[7..13] */

            /* 両方のキーをカウント */
            d->counts[pack_key7(left)]++;
            d->counts[pack_key7(right)]++;
        }
    }

    /*
     * ========================================
     * prefix sumでoffsetsを計算
     * ========================================
     */
    d->offsets = (int *)malloc(sizeof(int) * (size_t)(d->key_space + 1));
    d->offsets[0] = 0;
    for (int i = 0; i < d->key_space; ++i) {
        d->offsets[i + 1] = d->offsets[i] + (int)d->counts[i];
    }

    /* 総ID数を取得 */
    int total_ids = d->offsets[d->key_space];

    /* idpos配列を確保 */
    d->idpos = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)total_ids);

    /*
     * ========================================
     * Pass 2: 実際のidposをパックして格納
     * ========================================
     */
    int *cursor = (int *)malloc(sizeof(int) * (size_t)d->key_space);
    memcpy(cursor, d->offsets, sizeof(int) * (size_t)d->key_space);

    for (int id = 0; id < idx->keyword_count; ++id) {
        const char *w = idx->keywords[id];

        for (int pos = 0; pos < KEYWORD_LEN; ++pos) {
            /* pos位置を削除して14文字を作成 */
            for (int i = 0, k = 0; i < KEYWORD_LEN; ++i) {
                if (i == pos) continue;
                del[k++] = w[i];
            }

            /* 前半7文字と後半7文字に分割 */
            char left[7], right[7];
            memcpy(left, del, 7);
            memcpy(right, del + 7, 7);

            /* idとdel_posをパック
             * id: 下位20bit (0-1,048,575)
             * del_pos: bit 20-23 (0-14) */
            uint32_t packed_idpos = ((uint32_t)id & 0xFFFFFu) | ((uint32_t)pos << 20);

            /* 左キーのslot */
            int lslot = pack_key7(left);
            /* 右キーのslot */
            int rslot = pack_key7(right);

            /* 両方のslotにパック値を格納 */
            d->idpos[cursor[lslot]++] = packed_idpos;
            d->idpos[cursor[rslot]++] = packed_idpos;
        }
    }

    free(cursor);
}

/*
 * =============================================================================
 * 索引の確定
 * =============================================================================
 */

/*
 * casefilter_finalize: 索引を確定
 *
 * 【処理内容】
 * 全てのキーワード挿入が完了した後に呼び出される。
 * HIndexとDelIndexを構築し、検索可能な状態にする。
 *
 * 【呼び出しタイミング】
 * 1. casefilter_create()
 * 2. 複数回のcasefilter_insert()
 * 3. casefilter_finalize()  ← ここで索引を構築
 * 4. casefilter_search()    ← 検索可能に
 */
void casefilter_finalize(CaseFilterIndex *idx) {
    if (!idx) return;

    /* Case A用の索引を構築 */
    build_hindex(idx);

    /* Case B用の索引を構築 */
    build_dindex(idx);
}

/*
 * =============================================================================
 * シリアライズ
 * =============================================================================
 */

/*
 * serialize_hindex: HIndexをバイナリ形式で出力
 *
 * 【出力フォーマット】
 * 1. key_space (int): キー空間サイズ
 * 2. pair_count (int): ペア数
 * 3. count_bits (uint8_t): カウントのビット幅（16 or 32）
 * 4. counts[slots] (uint16_t or uint32_t): 各slotのカウント
 * 5. total_ids (int): 総ID数
 * 6. ids[total_ids] (3バイト×total_ids): キーワードID配列
 *
 * 【最適化: カウントの圧縮】
 * 最大カウントが65535以下なら16bitで保存、それ以外は32bit。
 * 100万件のキーワードでも、ほとんどのslotは数十件程度なので、
 * 16bitで十分な場合が多い。
 *
 * 【最適化: IDの3バイト圧縮】
 * キーワードIDは最大1,048,575（20bit）なので、3バイトで十分。
 * 通常の4バイト（int）ではなく、3バイトで保存することで25%削減。
 *
 * 例: id=123456 (0x0001E240)
 *     buf[0] = 0x40 (下位8bit)
 *     buf[1] = 0xE2 (中位8bit)
 *     buf[2] = 0x01 (上位8bit)
 */
static void serialize_hindex(const HIndex *hidx, FILE *out) {
    int slots = hidx->key_space * hidx->pair_count;

    /* ヘッダ情報を書き出し */
    fwrite(&hidx->key_space, sizeof(hidx->key_space), 1, out);
    fwrite(&hidx->pair_count, sizeof(hidx->pair_count), 1, out);

    /* 最大カウントを調べて、16bitで収まるか判定 */
    uint32_t maxc = 0;
    for (int i = 0; i < slots; ++i) {
        if (hidx->counts[i] > maxc) maxc = hidx->counts[i];
    }
    uint8_t count_bits = (maxc <= UINT16_MAX) ? 16 : 32;
    fwrite(&count_bits, 1, 1, out);

    /* カウント配列を書き出し */
    if (count_bits == 16) {
        /* 16bitに変換して書き出し（メモリ節約） */
        uint16_t *tmp = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)slots);
        for (int i = 0; i < slots; ++i) {
            tmp[i] = (uint16_t)hidx->counts[i];
        }
        fwrite(tmp, sizeof(uint16_t), (size_t)slots, out);
        free(tmp);
    } else {
        /* 32bitのまま書き出し */
        fwrite(hidx->counts, sizeof(uint32_t), (size_t)slots, out);
    }

    /* 総ID数を書き出し */
    int total_ids = hidx->offsets[slots];
    fwrite(&total_ids, sizeof(total_ids), 1, out);

    /* ID配列を3バイトずつ書き出し（メモリ節約） */
    for (int i = 0; i < total_ids; ++i) {
        uint32_t v = (uint32_t)hidx->ids[i];
        unsigned char buf[3] = {
            (unsigned char)(v & 0xFFu),        /* 下位8bit */
            (unsigned char)((v >> 8) & 0xFFu), /* 中位8bit */
            (unsigned char)((v >> 16) & 0xFFu) /* 上位8bit */
        };
        fwrite(buf, 1, 3, out);
    }
}

/*
 * serialize_dindex: DelIndexをバイナリ形式で出力
 *
 * 【出力フォーマット】
 * 1. key_space (int): キー空間サイズ
 * 2. count_bits (uint8_t): カウントのビット幅（16 or 32）
 * 3. counts[key_space] (uint16_t or uint32_t): 各キーのカウント
 * 4. total_ids (int): 総ID数
 * 5. idpos[total_ids] (3バイト×total_ids): パック済みidpos配列
 *
 * 【idposの3バイト圧縮】
 * idposは24bitあれば十分（id 20bit + del_pos 4bit）。
 * 32bitの下位24bitのみを保存することで、25%メモリ削減。
 *
 * 例: idpos = 0x00700001 (id=1, del_pos=7)
 *     buf[0] = 0x01 (下位8bit)
 *     buf[1] = 0x00 (中位8bit)
 *     buf[2] = 0x70 (上位8bit、del_pos含む)
 */
static void serialize_dindex(const DelIndex *didx, FILE *out) {
    /* ヘッダ情報 */
    fwrite(&didx->key_space, sizeof(didx->key_space), 1, out);

    /* 最大カウントを調べて、16bitで収まるか判定 */
    uint32_t maxc = 0;
    for (int i = 0; i < didx->key_space; ++i) {
        if (didx->counts[i] > maxc) maxc = didx->counts[i];
    }
    uint8_t count_bits = (maxc <= UINT16_MAX) ? 16 : 32;
    fwrite(&count_bits, 1, 1, out);

    /* カウント配列を書き出し */
    if (count_bits == 16) {
        uint16_t *tmp = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)didx->key_space);
        for (int i = 0; i < didx->key_space; ++i) {
            tmp[i] = (uint16_t)didx->counts[i];
        }
        fwrite(tmp, sizeof(uint16_t), (size_t)didx->key_space, out);
        free(tmp);
    } else {
        fwrite(didx->counts, sizeof(uint32_t), (size_t)didx->key_space, out);
    }

    /* 総ID数を書き出し */
    int total_ids = didx->offsets[didx->key_space];
    fwrite(&total_ids, sizeof(total_ids), 1, out);

    /* idpos配列を3バイトずつ書き出し */
    for (int i = 0; i < total_ids; ++i) {
        /* 下位24bitのみを保存 */
        uint32_t v = didx->idpos[i] & 0xFFFFFFu;
        unsigned char buf[3] = {
            (unsigned char)(v & 0xFFu),
            (unsigned char)((v >> 8) & 0xFFu),
            (unsigned char)((v >> 16) & 0xFFu)
        };
        fwrite(buf, 1, 3, out);
    }
}

/*
 * casefilter_serialize: 索引全体をシリアライズ
 *
 * 【出力フォーマット】
 * 1. keyword_count (int): キーワード数
 * 2. keywords[keyword_count] (char[16]×keyword_count): 生キーワード配列
 * 3. HIndex: serialize_hindex()
 * 4. DelIndex: serialize_dindex()
 *
 * 【注意】
 * codes配列は保存しない（デシリアライズ時にkeywordsから再計算）。
 * これにより、索引サイズをさらに削減できる。
 */
void casefilter_serialize(const CaseFilterIndex *idx, FILE *out) {
    if (!idx || !out) return;

    /* キーワード数を書き出し */
    fwrite(&idx->keyword_count, sizeof(idx->keyword_count), 1, out);

    /* キーワード配列を書き出し */
    fwrite(idx->keywords, sizeof(char[KEYWORD_LEN + 1]), idx->keyword_count, out);

    /* HIndexをシリアライズ */
    serialize_hindex(&idx->hidx, out);

    /* DelIndexをシリアライズ */
    serialize_dindex(&idx->del7, out);
}

/*
 * =============================================================================
 * デシリアライズ
 * =============================================================================
 */

/*
 * casefilter_deserialize: 索引をファイルから読み込み
 *
 * 【処理フロー】
 * 1. CaseFilterIndex構造体を確保
 * 2. keyword_countを読み込み
 * 3. keywords配列を読み込み
 * 4. codes配列を再計算（keywordsから）
 * 5. HIndexをデシリアライズ（counts → offsets → ids）
 * 6. DelIndexをデシリアライズ（counts → offsets → idpos）
 *
 * 【エラー処理】
 * 途中でエラーが発生した場合、確保したメモリを全て解放して NULL を返す。
 * goto fail パターンで統一的にクリーンアップ。
 *
 * 【codes配列の再計算】
 * シリアライズ時に保存しなかったcodes配列を、keywordsから再生成。
 * これにより、索引ファイルのサイズを削減しつつ、検索時の高速化を実現。
 */
CaseFilterIndex *casefilter_deserialize(FILE *in) {
    if (!in) return NULL;

    /* 構造体を確保＆ゼロ初期化 */
    CaseFilterIndex *idx = (CaseFilterIndex *)calloc(1, sizeof(CaseFilterIndex));

    /* キーワード数を読み込み */
    if (!fread_exact(&idx->keyword_count, sizeof(idx->keyword_count), 1, in)) {
        free(idx);
        return NULL;
    }

    idx->keyword_cap = idx->keyword_count;

    /* keywords配列とcodes配列を確保 */
    idx->keywords = (char (*)[KEYWORD_LEN + 1])malloc(
        sizeof(char[KEYWORD_LEN + 1]) * idx->keyword_cap
    );
    idx->codes = (uint64_t *)malloc(sizeof(uint64_t) * idx->keyword_cap);

    /* keywords配列を読み込み */
    if (!fread_exact(idx->keywords, sizeof(char[KEYWORD_LEN + 1]), idx->keyword_count, in)) {
        free(idx->keywords);
        free(idx->codes);
        free(idx);
        return NULL;
    }

    /* codes配列を再計算 */
    for (int i = 0; i < idx->keyword_count; ++i) {
        uint64_t code = 0;
        for (int j = 0; j < KEYWORD_LEN; ++j) {
            uint64_t nib = (uint64_t)(idx->keywords[i][j] - 'A') & 0xF;
            code |= nib << (j * 4);
        }
        idx->codes[i] = code;
    }

    /*
     * ========================================
     * HIndexのデシリアライズ
     * ========================================
     */
    uint8_t count_bits = 0;

    /* ヘッダ情報を読み込み */
    if (!fread_exact(&idx->hidx.key_space, sizeof(idx->hidx.key_space), 1, in)) goto fail;
    if (!fread_exact(&idx->hidx.pair_count, sizeof(idx->hidx.pair_count), 1, in)) goto fail;

    int h_slots = idx->hidx.key_space * idx->hidx.pair_count;

    if (!fread_exact(&count_bits, 1, 1, in)) goto fail;

    /* counts配列を読み込み（16bit or 32bit） */
    idx->hidx.counts = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)h_slots);
    if (count_bits == 16) {
        uint16_t *tmp = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)h_slots);
        if (!fread_exact(tmp, sizeof(uint16_t), h_slots, in)) {
            free(tmp);
            goto fail;
        }
        for (int i = 0; i < h_slots; ++i) {
            idx->hidx.counts[i] = tmp[i];
        }
        free(tmp);
    } else {
        if (!fread_exact(idx->hidx.counts, sizeof(uint32_t), h_slots, in)) goto fail;
    }

    /* offsets配列を計算（prefix sum） */
    idx->hidx.offsets = (int *)malloc(sizeof(int) * (size_t)(h_slots + 1));
    idx->hidx.offsets[0] = 0;
    for (int i = 0; i < h_slots; ++i) {
        idx->hidx.offsets[i + 1] = idx->hidx.offsets[i] + (int)idx->hidx.counts[i];
    }

    /* 総ID数を読み込み＆検証 */
    int h_total_ids_file = 0;
    if (!fread_exact(&h_total_ids_file, sizeof(h_total_ids_file), 1, in)) goto fail;
    int h_total_ids = idx->hidx.offsets[h_slots];
    if (h_total_ids != h_total_ids_file) goto fail;  /* 整合性チェック */

    /* ids配列を読み込み（3バイトずつ） */
    idx->hidx.ids = (int *)malloc(sizeof(int) * (size_t)h_total_ids);
    for (int i = 0; i < h_total_ids; ++i) {
        unsigned char buf[3];
        if (fread(buf, 1, 3, in) != 3) goto fail;
        /* 3バイトから32bit整数に復元 */
        idx->hidx.ids[i] = (int)(buf[0] | (buf[1] << 8) | (buf[2] << 16));
    }

    /*
     * ========================================
     * DelIndexのデシリアライズ
     * ========================================
     */
    if (!fread_exact(&idx->del7.key_space, sizeof(idx->del7.key_space), 1, in)) goto fail;
    if (!fread_exact(&count_bits, 1, 1, in)) goto fail;

    /* counts配列を読み込み */
    idx->del7.counts = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)idx->del7.key_space);
    if (count_bits == 16) {
        uint16_t *tmp = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)idx->del7.key_space);
        if (!fread_exact(tmp, sizeof(uint16_t), idx->del7.key_space, in)) {
            free(tmp);
            goto fail;
        }
        for (int i = 0; i < idx->del7.key_space; ++i) {
            idx->del7.counts[i] = tmp[i];
        }
        free(tmp);
    } else {
        if (!fread_exact(idx->del7.counts, sizeof(uint32_t), idx->del7.key_space, in)) goto fail;
    }

    /* offsets配列を計算 */
    idx->del7.offsets = (int *)malloc(sizeof(int) * (size_t)(idx->del7.key_space + 1));
    idx->del7.offsets[0] = 0;
    for (int i = 0; i < idx->del7.key_space; ++i) {
        idx->del7.offsets[i + 1] = idx->del7.offsets[i] + (int)idx->del7.counts[i];
    }

    /* 総ID数を読み込み＆検証 */
    int del_total_ids_file = 0;
    if (!fread_exact(&del_total_ids_file, sizeof(del_total_ids_file), 1, in)) goto fail;
    int del_total_ids = idx->del7.offsets[idx->del7.key_space];
    if (del_total_ids != del_total_ids_file) goto fail;

    /* idpos配列を読み込み（3バイトずつ） */
    idx->del7.idpos = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)del_total_ids);
    for (int i = 0; i < del_total_ids; ++i) {
        unsigned char buf[3];
        if (fread(buf, 1, 3, in) != 3) goto fail;
        /* 3バイトから32bit整数に復元 */
        idx->del7.idpos[i] = (uint32_t)(buf[0] | (buf[1] << 8) | (buf[2] << 16));
    }

    return idx;

fail:
    /* エラー時のクリーンアップ */
    casefilter_free(idx);
    return NULL;
}

/*
 * =============================================================================
 * 索引の解放
 * =============================================================================
 */

/*
 * casefilter_free: 索引の全メモリを解放
 *
 * 【解放対象】
 * 1. HIndex: offsets, counts, ids
 * 2. DelIndex: offsets, counts, idpos
 * 3. keywords, codes配列
 * 4. CaseFilterIndex構造体本体
 */
void casefilter_free(CaseFilterIndex *idx) {
    if (!idx) return;

    /* HIndexの配列を解放 */
    free(idx->hidx.offsets);
    free(idx->hidx.counts);
    free(idx->hidx.ids);

    /* DelIndexの配列を解放 */
    free(idx->del7.offsets);
    free(idx->del7.counts);
    free(idx->del7.idpos);

    /* キーワード関連の配列を解放 */
    free(idx->keywords);
    free(idx->codes);

    /* 構造体本体を解放 */
    free(idx);
}
