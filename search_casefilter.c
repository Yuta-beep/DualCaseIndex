#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ===== types.h の内容 ===== */
#define KEYWORD_LEN 15
#define MAX_EDIT_DIST 3
#define CHILD_BUCKETS (KEYWORD_LEN + 1)
#define INIT_CAPACITY 1024

/* ===== index.h の内容 ===== */
#define CASEFILTER_HPAIR_COUNT 10
#define CASEFILTER_H_KEY_SPACE 1000000
#define CASEFILTER_DEL_KEY_SPACE 10000000

typedef struct PostingH {
    char key[6];
    uint8_t pair_id;
    int *ids;
    int count;
    int cap;
    struct PostingH *next;
} PostingH;

typedef struct PostingDel {
    char key[7];
    int *ids;
    uint8_t *del_pos;
    int count;
    int cap;
    struct PostingDel *next;
} PostingDel;

typedef struct {
    int key_space;
    int pair_count;
    int *offsets;
    uint32_t *counts;
    int *ids;
} HIndex;

typedef struct {
    int key_space;
    int *offsets;
    uint32_t *counts;
    uint32_t *idpos;
} DelIndex;

typedef struct CaseFilterIndex {
    char (*keywords)[KEYWORD_LEN + 1];
    int keyword_count;
    int keyword_cap;
    uint64_t *codes;
    HIndex hidx;
    DelIndex del7;
} CaseFilterIndex;

CaseFilterIndex *casefilter_deserialize(FILE *in);
int casefilter_search(const CaseFilterIndex *idx, const char *query, int k);
void casefilter_free(CaseFilterIndex *idx);

static inline uint64_t casefilter_pack_delete(uint64_t code, int del_pos) {
    uint64_t low_mask = (del_pos == 0) ? 0 : ((1ULL << (del_pos * 4)) - 1ULL);
    uint64_t lower = code & low_mask;
    uint64_t upper = code >> ((del_pos + 1) * 4);
    return lower | (upper << (del_pos * 4));
}

/* ===== build.c のデシリアライズ部分 ===== */
static int fread_exact(void *dst, size_t size, size_t n, FILE *in) {
    return fread(dst, size, n, in) == n;
}

CaseFilterIndex *casefilter_deserialize(FILE *in) {
    if (!in) return NULL;
    CaseFilterIndex *idx = (CaseFilterIndex *)calloc(1, sizeof(CaseFilterIndex));
    if (!fread_exact(&idx->keyword_count, sizeof(idx->keyword_count), 1, in)) { free(idx); return NULL; }
    idx->keyword_cap = idx->keyword_count;
    idx->keywords = (char (*)[KEYWORD_LEN + 1])malloc(sizeof(char[KEYWORD_LEN + 1]) * idx->keyword_cap);
    idx->codes = (uint64_t *)malloc(sizeof(uint64_t) * idx->keyword_cap);
    if (!fread_exact(idx->keywords, sizeof(char[KEYWORD_LEN + 1]), idx->keyword_count, in)) {
        free(idx->keywords); free(idx->codes); free(idx); return NULL;
    }
    for (int i = 0; i < idx->keyword_count; ++i) {
        uint64_t code = 0;
        for (int j = 0; j < KEYWORD_LEN; ++j) {
            uint64_t nib = (uint64_t)(idx->keywords[i][j] - 'A') & 0xF;
            code |= nib << (j * 4);
        }
        idx->codes[i] = code;
    }

    /* HIndex deserialize (counts -> offsets -> ids) */
    uint8_t count_bits = 0;
    if (!fread_exact(&idx->hidx.key_space, sizeof(idx->hidx.key_space), 1, in)) goto fail;
    if (!fread_exact(&idx->hidx.pair_count, sizeof(idx->hidx.pair_count), 1, in)) goto fail;
    int h_slots = idx->hidx.key_space * idx->hidx.pair_count;
    if (!fread_exact(&count_bits, 1, 1, in)) goto fail;
    idx->hidx.counts = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)h_slots);
    if (count_bits == 16) {
        uint16_t *tmp = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)h_slots);
        if (!fread_exact(tmp, sizeof(uint16_t), h_slots, in)) { free(tmp); goto fail; }
        for (int i = 0; i < h_slots; ++i) idx->hidx.counts[i] = tmp[i];
        free(tmp);
    } else {
        if (!fread_exact(idx->hidx.counts, sizeof(uint32_t), h_slots, in)) goto fail;
    }
    idx->hidx.offsets = (int *)malloc(sizeof(int) * (size_t)(h_slots + 1));
    idx->hidx.offsets[0] = 0;
    for (int i = 0; i < h_slots; ++i) idx->hidx.offsets[i + 1] = idx->hidx.offsets[i] + (int)idx->hidx.counts[i];
    int h_total_ids_file = 0;
    if (!fread_exact(&h_total_ids_file, sizeof(h_total_ids_file), 1, in)) goto fail;
    int h_total_ids = idx->hidx.offsets[h_slots];
    if (h_total_ids != h_total_ids_file) goto fail;
    idx->hidx.ids = (int *)malloc(sizeof(int) * (size_t)h_total_ids);
    for (int i = 0; i < h_total_ids; ++i) {
        unsigned char buf[3];
        if (fread(buf, 1, 3, in) != 3) goto fail;
        idx->hidx.ids[i] = (int)(buf[0] | (buf[1] << 8) | (buf[2] << 16));
    }

    /* DelIndex deserialize */
    if (!fread_exact(&idx->del7.key_space, sizeof(idx->del7.key_space), 1, in)) goto fail;
    if (!fread_exact(&count_bits, 1, 1, in)) goto fail;
    idx->del7.counts = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)idx->del7.key_space);
    if (count_bits == 16) {
        uint16_t *tmp = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)idx->del7.key_space);
        if (!fread_exact(tmp, sizeof(uint16_t), idx->del7.key_space, in)) { free(tmp); goto fail; }
        for (int i = 0; i < idx->del7.key_space; ++i) idx->del7.counts[i] = tmp[i];
        free(tmp);
    } else {
        if (!fread_exact(idx->del7.counts, sizeof(uint32_t), idx->del7.key_space, in)) goto fail;
    }
    idx->del7.offsets = (int *)malloc(sizeof(int) * (size_t)(idx->del7.key_space + 1));
    idx->del7.offsets[0] = 0;
    for (int i = 0; i < idx->del7.key_space; ++i) idx->del7.offsets[i + 1] = idx->del7.offsets[i] + (int)idx->del7.counts[i];
    int del_total_ids_file = 0;
    if (!fread_exact(&del_total_ids_file, sizeof(del_total_ids_file), 1, in)) goto fail;
    int del_total_ids = idx->del7.offsets[idx->del7.key_space];
    if (del_total_ids != del_total_ids_file) goto fail;
    idx->del7.idpos = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)del_total_ids);
    for (int i = 0; i < del_total_ids; ++i) {
        unsigned char buf[3];
        if (fread(buf, 1, 3, in) != 3) goto fail;
        idx->del7.idpos[i] = (uint32_t)(buf[0] | (buf[1] << 8) | (buf[2] << 16));
    }
    return idx;

fail:
    casefilter_free(idx);
    return NULL;
}

void casefilter_free(CaseFilterIndex *idx) {
    if (!idx) return;
    free(idx->hidx.offsets);
    free(idx->hidx.counts);
    free(idx->hidx.ids);

    free(idx->del7.offsets);
    free(idx->del7.counts);
    free(idx->del7.idpos);

    free(idx->keywords);
    free(idx->codes);
    free(idx);
}

/* ===== search.c の内容 ===== */
static const uint8_t pair_i[10] = {0,0,0,0,1,1,1,2,2,3};
static const uint8_t pair_j[10] = {1,2,3,4,2,3,4,3,4,4};

/* 64bit範囲内でのビット並列popcount（HAKMEM法） */
static inline int popcount64(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (x * 0x0101010101010101ULL) >> 56;
}

/* SWAR Hamming: nibble-pack 15/14文字を popcount でまとめて判定 */
static inline uint64_t pack_keyword(const char *word) {
    uint64_t v = 0;
    for (int i = 0; i < KEYWORD_LEN; ++i) {
        uint64_t nib = (uint64_t)(word[i] - 'A') & 0xF;
        v |= nib << (i * 4);
    }
    return v;
}

static inline int hamming_packed15(uint64_t a, uint64_t b) {
    uint64_t x = a ^ b;
    x |= (x >> 1);
    x |= (x >> 2);
    x &= 0x1111111111111111ULL;
    return popcount64(x);
}

static inline int hamming_packed14(uint64_t a, uint64_t b) {
    uint64_t x = a ^ b;
    x |= (x >> 1);
    x |= (x >> 2);
    x &= 0x11111111111111ULL;  /* 14ニブル分のマスク（56bit） */
    return popcount64(x);
}

static inline uint32_t pack_key6(const char *key) {
    uint32_t v = 0;
    uint32_t mul = 1;
    for (int i = 0; i < 6; ++i) {
        v += (((uint32_t)key[i] - 'A') & 0xF) * mul;
        mul *= 10u;
    }
    return v;
}

static inline uint32_t pack_key7(const char *key) {
    uint32_t v = 0;
    uint32_t mul = 1;
    for (int i = 0; i < 7; ++i) {
        v += (((uint32_t)key[i] - 'A') & 0xF) * mul;
        mul *= 10u;
    }
    return v;
}

int casefilter_search(const CaseFilterIndex *idx, const char *query, int k) {
    if (!idx || !query || (int)strlen(query) != KEYWORD_LEN) return 0;
    int kw_count = idx->keyword_count;
    static uint32_t *visited_buf = NULL;
    static int visited_cap = 0;
    static uint32_t gen = 1;
    if (kw_count > visited_cap) {
        free(visited_buf);
        visited_buf = (uint32_t *)calloc((size_t)kw_count, sizeof(uint32_t));
        visited_cap = kw_count;
        gen = 1;
    }
    if (!visited_buf) return 0;
    gen++;
    if (gen == 0) {
        memset(visited_buf, 0, sizeof(uint32_t) * (size_t)visited_cap);
        gen = 1;
    }
    uint32_t *visited = visited_buf;

    /* Case A: Hamming<=3 via pair keys */
    char blocks[5][3];
    for (int b = 0; b < 5; ++b) memcpy(blocks[b], query + b * 3, 3);
    uint64_t qcode = pack_keyword(query);
    int order[10];
    struct { int start; int len; char key[6]; } pairs[10];
    for (int p = 0; p < 10; ++p) {
        order[p] = p;
        memcpy(pairs[p].key, blocks[pair_i[p]], 3);
        memcpy(pairs[p].key + 3, blocks[pair_j[p]], 3);
        uint32_t packed = pack_key6(pairs[p].key);
        int slot = (int)(packed + (uint32_t)p * CASEFILTER_H_KEY_SPACE);
        pairs[p].start = idx->hidx.offsets[slot];
        pairs[p].len = idx->hidx.offsets[slot + 1] - pairs[p].start;
    }
    /* sort keys by posting length (small→大) to早期ヒット狙い */
    for (int i = 0; i < 10; ++i) {
        for (int j = i + 1; j < 10; ++j) {
            if (pairs[order[i]].len > pairs[order[j]].len) {
                int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }
        }
    }

    for (int oi = 0; oi < 10; ++oi) {
        int p = order[oi];
        if (pairs[p].len == 0) continue;
        const int *ids = idx->hidx.ids + pairs[p].start;
        for (int i = 0; i < pairs[p].len; ++i) {
            int id = ids[i];
            if (visited[id] == gen) continue;
            visited[id] = gen;
            int hd = hamming_packed15(qcode, idx->codes[id]);
            if (hd <= k) return 1;
        }
    }

    gen++;

    /* Case B: delete-one -> 14char Hamming<=1 */
    char qdel[14];
    for (int pos = 0; pos < KEYWORD_LEN; ++pos) {
        uint64_t qdel_code = casefilter_pack_delete(qcode, pos);
        memcpy(qdel, query, pos);
        memcpy(qdel + pos, query + pos + 1, KEYWORD_LEN - pos - 1);
        char left[7], right[7];
        memcpy(left, qdel, 7);
        memcpy(right, qdel + 7, 7);

        int lslot = (int)pack_key7(left);
        int lstart = idx->del7.offsets[lslot];
        int lend = idx->del7.offsets[lslot + 1];
        if (lend > lstart) {
            const uint32_t *idpos = idx->del7.idpos + lstart;
            int count = lend - lstart;
            for (int i = 0; i < count; ++i) {
                uint32_t v = idpos[i];
                int id = (int)(v & 0xFFFFF);
                if (visited[id] == gen) continue;
                uint64_t kwdel_code = casefilter_pack_delete(idx->codes[id], (int)((v >> 20) & 0xF));
                int hd14 = hamming_packed14(qdel_code, kwdel_code);
                if (2 + hd14 <= k) {
                    visited[id] = gen;
                    return 1;
                }
            }
        }

        int rslot = (int)pack_key7(right);
        int rstart = idx->del7.offsets[rslot];
        int rend = idx->del7.offsets[rslot + 1];
        if (rend > rstart) {
            const uint32_t *idpos = idx->del7.idpos + rstart;
            int count = rend - rstart;
            for (int i = 0; i < count; ++i) {
                uint32_t v = idpos[i];
                int id = (int)(v & 0xFFFFF);
                if (visited[id] == gen) continue;
                uint64_t kwdel_code = casefilter_pack_delete(idx->codes[id], (int)((v >> 20) & 0xF));
                int hd14 = hamming_packed14(qdel_code, kwdel_code);
                if (2 + hd14 <= k) {
                    visited[id] = gen;
                    return 1;
                }
            }
        }
    }

    return 0;
}

/* ===== main/search_casefilter.c の main() ===== */
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <query_file> <index_file>\n", argv[0]);
        return 1;
    }
    FILE *idx = fopen(argv[2], "rb");
    if (!idx) {
        fprintf(stderr, "cannot open %s\n", argv[2]);
        return 1;
    }
    CaseFilterIndex *index = casefilter_deserialize(idx);
    fclose(idx);
    if (!index) {
        fprintf(stderr, "failed to load index\n");
        return 1;
    }

    FILE *qf = fopen(argv[1], "r");
    if (!qf) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        casefilter_free(index);
        return 1;
    }

    char buf[KEYWORD_LEN + 2];
    while (fgets(buf, sizeof(buf), qf)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        if ((int)strlen(buf) != KEYWORD_LEN) {
            putchar('0');
            continue;
        }
        int found = casefilter_search(index, buf, MAX_EDIT_DIST);
        putchar(found ? '1' : '0');
    }
    putchar('\n');

    fclose(qf);
    casefilter_free(index);
    return 0;
}
