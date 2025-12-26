# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

DualCaseIndex is a high-performance approximate string search system for edit distance ≤3 queries on fixed-length 15-character strings (alphabet A-J). The core innovation is converting Levenshtein distance computation into Hamming distance problems through case decomposition, avoiding expensive dynamic programming.

**Key Constraint**: Index size must fit within 200MB for large datasets (1M strings).

## Algorithm Architecture

### Two-Phase Index Strategy

The system decomposes edit distance ≤3 into two cases based on indel (insertion/deletion) count:

**Case A (indel=0)**: Pure substitutions → Hamming distance ≤3
- Splits each 15-char string into 5 blocks of 3 chars: `3/3/3/3/3`
- Generates 10 pair keys by concatenating each combination of 2 blocks (6 chars total)
- Pigeonhole principle: Hamming ≤3 guarantees at least 2 blocks match exactly
- Uses inverted index on pair keys for candidate retrieval

**Case B (indel=1)**: One deletion + one insertion
- For each string, generates 15 variants by deleting one character (14-char strings)
- Splits each 14-char variant into `7/7` blocks
- Maintains two index systems:
  - Left-7 key → Right-7 list
  - Right-7 key → Left-7 list
- Reduces to Hamming distance ≤1 problem on 14-char strings

### Search Process

1. Try Case A: Generate 10 pair keys from query, probe inverted index by bucket size (smallest first)
2. Filter with `hamming15` (≤3 check), confirm with `levenshtein_banded(max=3)`
3. If no match, try Case B: Generate 15 deletion variants, probe with left/right-7 keys
4. Filter with `hamming14` (≤1 check), confirm with `levenshtein_banded(max=3)`
5. Return 0 or 1 for each query

## Build Commands

### Core Binaries
```bash
# Index preparation
gcc -O2 prep_casefilter.c -o prep_casefilter

# Search execution
gcc -O2 search_casefilter.c -o search_casefilter

# Performance measurement tool
gcc -O2 record_perf.c -o record_perf
```

### Index Structure Files
- `prep_casefilter.c`: Unified index builder (includes types.h, index.h, build.c, main)
- `search_casefilter.c`: Unified search engine (includes types.h, index.h, build.c deserialization, search.c, main)
- Both files are **monolithic** - they inline all necessary code for standalone compilation

## Running Experiments

### Basic Workflow
```bash
# Build index from database
./prep_casefilter test-data/db_1 > output/index_casefilter_1

# Execute queries against index
./search_casefilter test-data/query_1 output/index_casefilter_1 > output/result_casefilter
```

### Performance Measurement
```bash
# Time measurement only
/usr/bin/time -f 'search %e' ./search_casefilter test-data/query_100k output/index_casefilter_100k > /dev/null

# Full performance logging to CSV
./record_perf --record -- ./search_casefilter test-data/query_100k output/index_casefilter_100k > /dev/null
```

Performance logs are written to `records/perf_*.csv` with timestamp and metrics.

### Validation
```bash
# Validate search results against naive Levenshtein implementation
./validate.py --prep-bin ./prep_casefilter \
              --search-bin ./search_casefilter \
              --db test-data/db_1 \
              --query test-data/query_1

# Skip index rebuild if already exists
./validate.py --search-bin ./search_casefilter \
              --db test-data/db_1 \
              --query test-data/query_1 \
              --index output/index_casefilter_1
```

The validator runs Python-based naive Levenshtein matching and compares bit-by-bit output.

## Test Data Structure

Files in `test-data/` follow the pattern:
- `db_N`: Database files (N = 1, 2, 3, 10k, 20k, 100k, 500k)
- `query_N`: Corresponding query files

Each line contains one 15-character string using alphabet A-J.

## Key Data Structures

### Index Components
- **HIndex**: Case A (Hamming) inverted index with 1M key space, 10 pair IDs
- **DelIndex**: Case B (deletion) inverted index with 10M key space for left/right-7 keys
- **CaseFilterIndex**: Top-level container holding both HIndex and DelIndex

### Posting Lists
- **PostingH**: Stores (6-char key, pair_id, id_list) for Case A
- **PostingDel**: Stores (7-char key, id_list, deletion_positions) for Case B

## Important Constants

```c
#define KEYWORD_LEN 15              // Fixed string length
#define MAX_EDIT_DIST 3             // Maximum edit distance
#define CASEFILTER_HPAIR_COUNT 10   // Number of pair keys (C(5,2))
#define CASEFILTER_H_KEY_SPACE 1000000    // Case A key space
#define CASEFILTER_DEL_KEY_SPACE 10000000 // Case B key space
```

## Optimization Priorities

1. **Index Size**: Primary constraint is 200MB for 1M strings
   - Consider CSR (Compressed Sparse Row) format for posting lists
   - Bit-pack IDs (assume 20-bit IDs for 1M entries)
   - Compress bucket metadata

2. **Search Speed**: Optimize bucket traversal order
   - Track bucket size statistics during build
   - Probe smallest buckets first for early termination
   - Implement adaptive thresholds based on query patterns

3. **Code Quality**: Fix compilation warnings
   - Add `fread` return value checks
   - Validate all file I/O operations

## Language and Cultural Context

- Documentation is primarily in Japanese
- Code comments mix English technical terms with Japanese explanations
- "indel" = insertion/deletion operations
- "帯域付き" (banded) = banded Levenshtein for early termination
