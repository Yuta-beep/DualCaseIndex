/*
 * =============================================================================
 * casefilter アルゴリズム - データ構造定義
 * =============================================================================
 *
 * 【概要】
 * このヘッダファイルは、casefilter アルゴリズムで使用する全てのデータ構造と
 * 主要な関数シグネチャを定義している。
 *
 * 【アルゴリズムの基本原理】
 * 固定長15文字のキーワードに対する編集距離≤3の類似検索を高速化するために、
 * 編集距離を以下の2つのケースに分解する：
 *
 * - Case A (indel=0): 純粋な置換のみ
 *   → Hamming距離≤3 で判定可能
 *   → 15文字を3文字×5ブロックに分割し、10通りのペア索引を構築
 *
 * - Case B (indel=1): 1削除+1挿入（+置換≤1）
 *   → 削除後の14文字間のHamming距離≤1 で判定可能
 *   → コスト = 2 (削除+挿入) + Hamming14
 *   → 14文字を前半7文字+後半7文字に分けて索引化
 *
 * 固定長15文字の制約により、挿入回数=削除回数が保証される。
 * indel≥2 の場合は編集距離≥4となり、k=3の対象外。
 */

#ifndef CASEFILTER_INDEX_H
#define CASEFILTER_INDEX_H

#include <stdio.h>
#include "../../common/types.h"

/*
 * =============================================================================
 * 定数定義
 * =============================================================================
 */

/* Case A用: 6文字ペアの組み合わせ数
 * 15文字を3文字×5ブロックに分けた時、C(5,2) = 10 通りのペアがある */
#define CASEFILTER_HPAIR_COUNT 10

/* Case A用: 6文字キーのキー空間サイズ
 * 6文字 × 10通り（A-J） = 10^6 = 1,000,000
 * 各文字をA=0, B=1, ..., J=9として10進数で表現する */
#define CASEFILTER_H_KEY_SPACE 1000000

/* Case B用: 7文字キーのキー空間サイズ
 * 7文字 × 10通り（A-J） = 10^7 = 10,000,000
 * 削除後の14文字を前半7文字と後半7文字に分けて、同じ索引に格納する */
#define CASEFILTER_DEL_KEY_SPACE 10000000

/*
 * =============================================================================
 * Case A用データ構造（Hamming索引）
 * =============================================================================
 */

/*
 * PostingH: Case A用の中間データ構造（構築時のみ使用）
 *
 * 【用途】
 * 索引構築時に、6文字キーごとのポスティングリストを一時的に保持する。
 * 最終的にはHIndexの圧縮形式に変換される。
 *
 * 【構造】
 * - key[6]: 6文字のキー（3文字×2ブロックを連結したもの）
 * - pair_id: どのペア（0-9）かを示すID
 * - ids[]: このキーに該当するキーワードIDの配列
 * - count: 現在格納されているID数
 * - cap: 配列の容量
 * - next: ハッシュテーブルのチェイン（衝突解決用）
 */
typedef struct PostingH {
    char key[6];           /* 6文字キー */
    uint8_t pair_id;       /* ペアID（0-9） */
    int *ids;              /* キーワードID配列 */
    int count;             /* 現在のID数 */
    int cap;               /* 配列容量 */
    struct PostingH *next; /* チェインポインタ */
} PostingH;

/*
 * PostingDel: Case B用の中間データ構造（構築時のみ使用）
 *
 * 【用途】
 * 索引構築時に、7文字キーごとのポスティングリストを一時的に保持する。
 * 最終的にはDelIndexの圧縮形式に変換される。
 *
 * 【構造】
 * - key[7]: 7文字のキー（削除後14文字の前半または後半）
 * - ids[]: このキーに該当するキーワードIDの配列
 * - del_pos[]: 各IDに対応する削除位置（0-14）
 * - count: 現在格納されているID数
 * - cap: 配列容量
 * - next: ハッシュテーブルのチェイン
 */
typedef struct PostingDel {
    char key[7];           /* 7文字キー */
    int *ids;              /* キーワードID配列 */
    uint8_t *del_pos;      /* 削除位置配列（0-14） */
    int count;             /* 現在のID数 */
    int cap;               /* 配列容量 */
    struct PostingDel *next; /* チェインポインタ */
} PostingDel;

/*
 * HIndex: Case A用の圧縮済み索引（検索時に使用）
 *
 * 【設計原理】
 * Compressed Sparse Row (CSR) 形式で、6文字キーから直接ポスティングリストに
 * アクセスできるようにする。ハッシュテーブルは使わず、直接アドレッシング。
 *
 * 【データ構造の詳細】
 * offsets配列とids配列を使って、以下のようにポスティングリストを表現：
 * - offsets[slot] から offsets[slot+1]-1 までが、そのslotのポスティング範囲
 * - ids[offsets[slot]...offsets[slot+1]-1] に実際のキーワードIDが格納
 *
 * 【slot番号の計算】
 * slot = pack_key6(6文字キー) + pair_id × H_KEY_SPACE
 * 例: キー"ABCDEF", pair_id=3 の場合
 *     slot = pack_key6("ABCDEF") + 3 × 1,000,000
 *
 * 【メモリレイアウト例】
 * offsets: [0, 3, 3, 7, 10, ...]  (各slotのポスティング開始位置)
 * ids:     [5, 12, 23, 8, 9, 15, 20, 1, 3, 7, ...]  (連結されたキーワードID)
 *
 * slot 0のポスティング: ids[0..2] = [5, 12, 23]  (3件)
 * slot 1のポスティング: なし  (0件)
 * slot 2のポスティング: ids[3..6] = [8, 9, 15, 20]  (4件)
 * slot 3のポスティング: ids[7..9] = [1, 3, 7]  (3件)
 */
typedef struct {
    int key_space;    /* キー空間サイズ（固定: 1,000,000） */
    int pair_count;   /* ペア数（固定: 10） */
    int *offsets;     /* サイズ: key_space*pair_count + 1
                       * 各slotのポスティング開始位置を格納
                       * offsets[slot+1] - offsets[slot] がそのslotの件数 */
    uint32_t *counts; /* サイズ: key_space*pair_count
                       * 各slotの件数（シリアライズ用に保持） */
    int *ids;         /* 全ポスティングを連結した配列
                       * サイズ: offsets[key_space*pair_count] */
} HIndex;

/*
 * DelIndex: Case B用の圧縮済み索引（検索時に使用）
 *
 * 【設計原理】
 * HIndexと同様にCSR形式だが、以下の点が異なる：
 * 1. 7文字キーを使用（14文字を前半7+後半7に分割）
 * 2. idとdel_posを1つの32bit値にパック（id: 下位20bit, del_pos: 上位4bit）
 *
 * 【idposのビット構成】
 * 32bit = [unused 8bit][del_pos 4bit][id 20bit]
 *         bits 24-31   bits 20-23    bits 0-19
 *
 * 例: id=123456, del_pos=7 の場合
 *     idpos = (123456 & 0xFFFFF) | (7 << 20) = 0x00700001E240
 *
 * 【前半7文字と後半7文字の統合】
 * 削除後の14文字を前半7文字（left）と後半7文字（right）に分けて、
 * 同じDelIndexに格納する。検索時は両方のキーで索引を引く。
 *
 * 例: キーワード "ABCDEFGHIJKLMNO" から位置3を削除
 *     削除後: "ABCEFGHIJKLMNO" (14文字)
 *     left:  "ABCEFGH" (前半7文字)
 *     right: "IJKLMNO" (後半7文字)
 *     両方のキーで索引に登録（同じid=?、del_pos=3）
 *
 * 【なぜ左右を統合するか】
 * キー空間が同じ（10^7）なので、別々の索引を持つ必要がない。
 * 1つの索引に統合することでメモリを節約できる。
 */
typedef struct {
    int key_space;    /* キー空間サイズ（固定: 10,000,000） */
    int *offsets;     /* サイズ: key_space+1
                       * 各7文字キーのポスティング開始位置 */
    uint32_t *counts; /* サイズ: key_space
                       * 各キーの件数（シリアライズ用） */
    uint32_t *idpos;  /* パック済みポスティング配列
                       * サイズ: offsets[key_space]
                       * 各要素は (id 20bit | del_pos 4bit) */
} DelIndex;

/*
 * =============================================================================
 * メイン索引構造
 * =============================================================================
 */

/*
 * CaseFilterIndex: casefilterアルゴリズムの完全な索引
 *
 * 【構造】
 * 1. キーワード本体とSWAR用パック配列
 * 2. Case A用の索引（HIndex）
 * 3. Case B用の索引（DelIndex）
 *
 * 【SWAR (SIMD Within A Register)】
 * 各キーワードを15文字×4bit=60bitにパックして1つのuint64_tに格納。
 * これにより、Hamming距離計算が高速化される：
 *
 * 文字 'A' 'B' 'C' 'D' 'E' ... (15文字)
 *  ↓
 * 4bit  0   1   2   3   4  ...
 *  ↓
 * codes[id] = 0x0000...0000043210  (60bit使用)
 *
 * Hamming計算: XORしてビット演算で一度に処理
 * 通常の実装: O(15) の文字比較ループ
 * SWAR実装: O(1) のビット演算（__builtin_popcountll）
 */
typedef struct CaseFilterIndex {
    char (*keywords)[KEYWORD_LEN + 1]; /* キーワード配列（16バイト×n件） */
    int keyword_count;                 /* 現在登録されているキーワード数 */
    int keyword_cap;                   /* 配列容量 */
    uint64_t *codes;                   /* SWAR用4bitパック配列
                                        * 各要素は15文字を60bitにパック
                                        * 高速Hamming計算に使用 */
    HIndex hidx;                       /* Case A用索引
                                        * 6文字ペア×10通り = 10M slots */
    DelIndex del7;                     /* Case B用索引
                                        * 7文字キー（左右統合） = 10M slots */
} CaseFilterIndex;

/*
 * =============================================================================
 * 公開API関数
 * =============================================================================
 */

/* 索引の作成（容量を指定） */
CaseFilterIndex *casefilter_create(int capacity);

/* キーワードの挿入（索引構築前） */
void casefilter_insert(CaseFilterIndex *idx, const char *word);

/* 索引の確定（全キーワード挿入後に呼び出す）
 * HIndexとDelIndexを構築し、CSR形式に圧縮する */
void casefilter_finalize(CaseFilterIndex *idx);

/* 索引のシリアライズ（ファイルへの保存） */
void casefilter_serialize(const CaseFilterIndex *idx, FILE *out);

/* 索引のデシリアライズ（ファイルからの読み込み） */
CaseFilterIndex *casefilter_deserialize(FILE *in);

/* 類似検索（編集距離≤kのキーワードが存在するか）
 * 戻り値: 1=見つかった, 0=見つからなかった */
int casefilter_search(const CaseFilterIndex *idx, const char *query, int k);

/* 索引の解放 */
void casefilter_free(CaseFilterIndex *idx);

/*
 * =============================================================================
 * ユーティリティ関数
 * =============================================================================
 */

/*
 * casefilter_pack_delete: 4bitパックコードから指定位置を削除
 *
 * 【用途】
 * Case Bの検索で使用。15文字のパックコードから1文字を削除し、
 * 14文字のパックコード（56bit）を生成する。
 *
 * 【引数】
 * - code: 15文字の4bitパックコード（60bit使用）
 * - del_pos: 削除する位置（0-14）
 *
 * 【戻り値】
 * 削除後の14文字パックコード（56bit使用）
 *
 * 【アルゴリズム】
 * 1. del_posより下位のビットを取得（lower）
 * 2. del_posより上位のビットを取得して4bit左シフト（upper）
 * 3. lowerとupperを連結
 *
 * 【具体例】
 * code = "ABCDE" = 0x43210 (各文字4bit)
 * del_pos = 2 ('C'を削除)
 *
 * low_mask = (1 << (2*4)) - 1 = 0xFF (下位8bit)
 * lower = code & 0xFF = 0x10 ('A', 'B')
 * upper = code >> ((2+1)*4) = code >> 12 = 0x43 ('D', 'E')
 * 結果 = 0x10 | (0x43 << 8) = 0x4310 ('A', 'B', 'D', 'E')
 */
static inline uint64_t casefilter_pack_delete(uint64_t code, int del_pos) {
    /* del_pos位置より下位のビットマスクを生成
     * del_pos=0の場合はマスク=0（下位ビットなし） */
    uint64_t low_mask = (del_pos == 0) ? 0 : ((1ULL << (del_pos * 4)) - 1ULL);

    /* 下位部分（del_posより前）を取得 */
    uint64_t lower = code & low_mask;

    /* 上位部分（del_posより後）を取得して4bit左にシフト
     * del_posの4bitをスキップする */
    uint64_t upper = code >> ((del_pos + 1) * 4);

    /* 下位と上位を連結
     * upperを元の位置（del_posの位置）に配置 */
    return lower | (upper << (del_pos * 4));
}

#endif /* CASEFILTER_INDEX_H */
