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

CaseFilterIndex *casefilter_create(int capacity);
void casefilter_insert(CaseFilterIndex *idx, const char *word);
void casefilter_finalize(CaseFilterIndex *idx);
void casefilter_serialize(const CaseFilterIndex *idx, FILE *out);
void casefilter_free(CaseFilterIndex *idx);

static inline uint64_t casefilter_pack_delete(uint64_t code, int del_pos) {
    uint64_t low_mask = (del_pos == 0) ? 0 : ((1ULL << (del_pos * 4)) - 1ULL);
    uint64_t lower = code & low_mask;
    uint64_t upper = code >> ((del_pos + 1) * 4);
    return lower | (upper << (del_pos * 4));
}

/* ===== build.c の内容 ===== */
#define HPAIR_COUNT CASEFILTER_HPAIR_COUNT
#define H_KEY_SPACE CASEFILTER_H_KEY_SPACE
#define DEL_KEY_SPACE CASEFILTER_DEL_KEY_SPACE

static const uint8_t pair_i[HPAIR_COUNT] = {0,0,0,0,1,1,1,2,2,3};
static const uint8_t pair_j[HPAIR_COUNT] = {1,2,3,4,2,3,4,3,4,4};

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

static int fread_exact(void *dst, size_t size, size_t n, FILE *in) {
    return fread(dst, size, n, in) == n;
}

static void ensure_keyword_cap(CaseFilterIndex *idx) {
    if (idx->keyword_count < idx->keyword_cap) return;
    int new_cap = idx->keyword_cap ? idx->keyword_cap * 2 : 1024;
    idx->keywords = (char (*)[KEYWORD_LEN + 1])realloc(idx->keywords, sizeof(char[KEYWORD_LEN + 1]) * new_cap);
    idx->codes = (uint64_t *)realloc(idx->codes, sizeof(uint64_t) * new_cap);
    idx->keyword_cap = new_cap;
}

CaseFilterIndex *casefilter_create(int capacity) {
    CaseFilterIndex *idx = (CaseFilterIndex *)calloc(1, sizeof(CaseFilterIndex));
    idx->keyword_cap = capacity > 0 ? capacity : 1024;
    idx->keywords = (char (*)[KEYWORD_LEN + 1])malloc(sizeof(char[KEYWORD_LEN + 1]) * idx->keyword_cap);
    idx->codes = (uint64_t *)malloc(sizeof(uint64_t) * idx->keyword_cap);
    return idx;
}

void casefilter_insert(CaseFilterIndex *idx, const char *word) {
    if (!idx || !word) return;
    ensure_keyword_cap(idx);
    memcpy(idx->keywords[idx->keyword_count], word, KEYWORD_LEN);
    idx->keywords[idx->keyword_count][KEYWORD_LEN] = '\0';
    /* pack to 4bit per char for SWAR Hamming */
    uint64_t code = 0;
    for (int i = 0; i < KEYWORD_LEN; ++i) {
        uint64_t nib = (uint64_t)(word[i] - 'A') & 0xF;
        code |= nib << (i * 4);
    }
    idx->codes[idx->keyword_count] = code;
    idx->keyword_count++;
}

static void build_hindex(CaseFilterIndex *idx) {
    HIndex *h = &idx->hidx;
    h->key_space = H_KEY_SPACE;
    h->pair_count = HPAIR_COUNT;
    int slots = h->key_space * h->pair_count;
    h->counts = (uint32_t *)calloc((size_t)slots, sizeof(uint32_t));
    /* 1st pass: count */
    for (int id = 0; id < idx->keyword_count; ++id) {
        const char *w = idx->keywords[id];
        char blocks[5][3];
        for (int b = 0; b < 5; ++b) memcpy(blocks[b], w + b * 3, 3);
        for (int p = 0; p < HPAIR_COUNT; ++p) {
            char key[6];
            memcpy(key, blocks[pair_i[p]], 3);
            memcpy(key + 3, blocks[pair_j[p]], 3);
            int slot = (int)(pack_key6(key) + (uint32_t)p * H_KEY_SPACE);
            h->counts[slot]++;
        }
    }
    /* prefix sum -> offsets */
    h->offsets = (int *)malloc(sizeof(int) * (size_t)(slots + 1));
    h->offsets[0] = 0;
    for (int i = 0; i < slots; ++i) {
        h->offsets[i + 1] = h->offsets[i] + (int)h->counts[i];
    }
    int total_ids = h->offsets[slots];
    h->ids = (int *)malloc(sizeof(int) * (size_t)total_ids);
    int *cursor = (int *)malloc(sizeof(int) * (size_t)slots);
    memcpy(cursor, h->offsets, sizeof(int) * (size_t)slots);
    /* 2nd pass: fill postings */
    for (int id = 0; id < idx->keyword_count; ++id) {
        const char *w = idx->keywords[id];
        char blocks[5][3];
        for (int b = 0; b < 5; ++b) memcpy(blocks[b], w + b * 3, 3);
        for (int p = 0; p < HPAIR_COUNT; ++p) {
            char key[6];
            memcpy(key, blocks[pair_i[p]], 3);
            memcpy(key + 3, blocks[pair_j[p]], 3);
            int slot = (int)(pack_key6(key) + (uint32_t)p * H_KEY_SPACE);
            h->ids[cursor[slot]++] = id;
        }
    }
    free(cursor);
}

static void build_dindex(CaseFilterIndex *idx) {
    DelIndex *d = &idx->del7;
    d->key_space = DEL_KEY_SPACE;
    d->counts = (uint32_t *)calloc((size_t)d->key_space, sizeof(uint32_t));
    /* 1st pass: count */
    char del[14];
    for (int id = 0; id < idx->keyword_count; ++id) {
        const char *w = idx->keywords[id];
        for (int pos = 0; pos < KEYWORD_LEN; ++pos) {
            for (int i = 0, k = 0; i < KEYWORD_LEN; ++i) {
                if (i == pos) continue;
                del[k++] = w[i];
            }
            char left[7], right[7];
            memcpy(left, del, 7);
            memcpy(right, del + 7, 7);
            d->counts[pack_key7(left)]++;
            d->counts[pack_key7(right)]++;
        }
    }
    /* prefix sum -> offsets */
    d->offsets = (int *)malloc(sizeof(int) * (size_t)(d->key_space + 1));
    d->offsets[0] = 0;
    for (int i = 0; i < d->key_space; ++i) {
        d->offsets[i + 1] = d->offsets[i] + (int)d->counts[i];
    }
    int total_ids = d->offsets[d->key_space];
    d->idpos = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)total_ids);
    int *cursor = (int *)malloc(sizeof(int) * (size_t)d->key_space);
    memcpy(cursor, d->offsets, sizeof(int) * (size_t)d->key_space);
    /* 2nd pass: fill postings */
    for (int id = 0; id < idx->keyword_count; ++id) {
        const char *w = idx->keywords[id];
        for (int pos = 0; pos < KEYWORD_LEN; ++pos) {
            for (int i = 0, k = 0; i < KEYWORD_LEN; ++i) {
                if (i == pos) continue;
                del[k++] = w[i];
            }
            char left[7], right[7];
            memcpy(left, del, 7);
            memcpy(right, del + 7, 7);
            uint32_t packed_idpos = ((uint32_t)id & 0xFFFFFu) | ((uint32_t)pos << 20);
            int lslot = pack_key7(left);
            int rslot = pack_key7(right);
            d->idpos[cursor[lslot]++] = packed_idpos;
            d->idpos[cursor[rslot]++] = packed_idpos;
        }
    }
    free(cursor);
}

void casefilter_finalize(CaseFilterIndex *idx) {
    if (!idx) return;
    build_hindex(idx);
    build_dindex(idx);
}

static void serialize_hindex(const HIndex *hidx, FILE *out) {
    int slots = hidx->key_space * hidx->pair_count;
    fwrite(&hidx->key_space, sizeof(hidx->key_space), 1, out);
    fwrite(&hidx->pair_count, sizeof(hidx->pair_count), 1, out);
    uint32_t maxc = 0;
    for (int i = 0; i < slots; ++i) if (hidx->counts[i] > maxc) maxc = hidx->counts[i];
    uint8_t count_bits = (maxc <= UINT16_MAX) ? 16 : 32;
    fwrite(&count_bits, 1, 1, out);
    if (count_bits == 16) {
        uint16_t *tmp = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)slots);
        for (int i = 0; i < slots; ++i) tmp[i] = (uint16_t)hidx->counts[i];
        fwrite(tmp, sizeof(uint16_t), (size_t)slots, out);
        free(tmp);
    } else {
        fwrite(hidx->counts, sizeof(uint32_t), (size_t)slots, out);
    }
    int total_ids = hidx->offsets[slots];
    fwrite(&total_ids, sizeof(total_ids), 1, out);
    /* ids fit in 20bit (0..1e6), write as 3-byte little-endian to save space */
    for (int i = 0; i < total_ids; ++i) {
        uint32_t v = (uint32_t)hidx->ids[i];
        unsigned char buf[3] = {(unsigned char)(v & 0xFFu),
                                (unsigned char)((v >> 8) & 0xFFu),
                                (unsigned char)((v >> 16) & 0xFFu)};
        fwrite(buf, 1, 3, out);
    }
}

static void serialize_dindex(const DelIndex *didx, FILE *out) {
    fwrite(&didx->key_space, sizeof(didx->key_space), 1, out);
    uint32_t maxc = 0;
    for (int i = 0; i < didx->key_space; ++i) if (didx->counts[i] > maxc) maxc = didx->counts[i];
    uint8_t count_bits = (maxc <= UINT16_MAX) ? 16 : 32;
    fwrite(&count_bits, 1, 1, out);
    if (count_bits == 16) {
        uint16_t *tmp = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)didx->key_space);
        for (int i = 0; i < didx->key_space; ++i) tmp[i] = (uint16_t)didx->counts[i];
        fwrite(tmp, sizeof(uint16_t), (size_t)didx->key_space, out);
        free(tmp);
    } else {
        fwrite(didx->counts, sizeof(uint32_t), (size_t)didx->key_space, out);
    }
    int total_ids = didx->offsets[didx->key_space];
    fwrite(&total_ids, sizeof(total_ids), 1, out);
    /* idpos packed to 3 bytes (id20bit | del_pos4bit) */
    for (int i = 0; i < total_ids; ++i) {
        uint32_t v = didx->idpos[i] & 0xFFFFFFu;
        unsigned char buf[3] = {(unsigned char)(v & 0xFFu),
                                (unsigned char)((v >> 8) & 0xFFu),
                                (unsigned char)((v >> 16) & 0xFFu)};
        fwrite(buf, 1, 3, out);
    }
}

void casefilter_serialize(const CaseFilterIndex *idx, FILE *out) {
    if (!idx || !out) return;
    fwrite(&idx->keyword_count, sizeof(idx->keyword_count), 1, out);
    fwrite(idx->keywords, sizeof(char[KEYWORD_LEN + 1]), idx->keyword_count, out);
    serialize_hindex(&idx->hidx, out);
    serialize_dindex(&idx->del7, out);
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

/* ===== main/prep_casefilter.c の main() ===== */
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <db_file>\n", argv[0]);
        return 1;
    }
    FILE *fp = fopen(argv[1], "r");
    if (!fp) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }

    CaseFilterIndex *index = casefilter_create(INIT_CAPACITY);
    char buf[KEYWORD_LEN + 2];
    while (fgets(buf, sizeof(buf), fp)) {
        if (buf[0] == '\0') continue;
        buf[strcspn(buf, "\r\n")] = '\0';
        if ((int)strlen(buf) != KEYWORD_LEN) continue;
        casefilter_insert(index, buf);
    }
    fclose(fp);

    casefilter_finalize(index);
    casefilter_serialize(index, stdout);
    casefilter_free(index);
    return 0;
}
