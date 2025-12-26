# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**DualCaseIndex** is a high-performance approximate string matching system for fixed-length keywords (15 characters, alphabet A-J). It uses a case decomposition approach to solve edit distance ≤3 queries without computing full Levenshtein DP, transforming the problem into Hamming distance checks plus partitioned hashing.

### Core Algorithm

The system decomposes edit distance queries based on indel (insertion/deletion) count:
- **Case A (indel=0)**: Pure substitutions → Hamming distance ≤3
- **Case B (indel=1)**: 1 deletion + 1 insertion → After removing one character from both strings, becomes 14-char Hamming distance ≤1

This decomposition is exact for edit distance ≤3 on fixed-length strings because:
- Fixed length means insertions = deletions
- Distance formula: `edit = substitutions + 2*indel`
- indel ≥2 would give distance ≥4, outside the threshold

## Build Commands

### Build Preparation Binary
```bash
gcc -O2 -lm src/casefilter/build.c src/main/prep_casefilter.c -o prep_casefilter
```

### Build Search Binary
```bash
gcc -O2 -lm src/casefilter/build.c src/casefilter/search.c src/main/search_casefilter.c -o search_casefilter
```

### Build Performance Recording Utility
```bash
gcc -O2 src/util/record_perf.c -o record_perf
```

## Running the System

### Index Construction
```bash
./prep_casefilter test-data/db_1 > output/index_casefilter_1
```

### Search Execution
```bash
./search_casefilter test-data/query_1 output/index_casefilter_1 > output/result_casefilter
```

### Performance Measurement
```bash
# Using /usr/bin/time
/usr/bin/time -f 'search %e' ./search_casefilter test-data/query_100k output/index_casefilter_100k > /dev/null

# Using record_perf utility
./record_perf --record -- ./search_casefilter test-data/query_100k output/index_casefilter_100k > /dev/null
```

## Testing and Validation

### Run Validation Tests
```bash
python3 src/tests/validate.py \
    --prep-bin ./prep_casefilter \
    --search-bin ./search_casefilter \
    --db test-data/db_1 \
    --query test-data/query_1
```

### Test with Existing Index
```bash
python3 src/tests/validate.py \
    --search-bin ./search_casefilter \
    --db test-data/db_1 \
    --query test-data/query_1 \
    --index output/index_casefilter_1
```

### Sample Validation (Limited Dataset)
```bash
python3 src/tests/validate.py \
    --prep-bin ./prep_casefilter \
    --search-bin ./search_casefilter \
    --db test-data/db_100k \
    --query test-data/query_100k \
    --limit-db 1000 \
    --limit-query 100
```

## Architecture

### Directory Structure

```
src/
├── casefilter/           # Main algorithm implementation
│   ├── index.h           # Data structure definitions and signatures
│   ├── build.c           # Index construction and serialization
│   ├── search.c          # Search algorithm (case decomposition + Hamming)
│   └── ALGORITHM.md      # Detailed algorithm documentation (Japanese)
├── main/                 # Entry points
│   ├── prep_casefilter.c    # Index construction binary
│   └── search_casefilter.c  # Search binary
├── util/                 # Shared utilities
│   ├── types.h           # Common constants (KEYWORD_LEN=15, MAX_EDIT_DIST=3)
│   ├── levenshtein.h     # Levenshtein implementations (banded DP, Myers bit-parallel)
│   └── record_perf.c     # Performance recording utility
└── tests/
    └── validate.py       # Validation script against naive Levenshtein
```

### Index Data Structures

#### HIndex (Case A: indel=0)
- **Purpose**: Handle pure substitutions (Hamming distance ≤3)
- **Strategy**: Partition 15-char string into 5 blocks of 3 chars each
- **Key insight**: Hamming ≤3 requires ≥2 matching blocks (pigeonhole principle)
- **Implementation**: 10 inverted indices (one per pair from C(5,2)=10 combinations)
- **Key format**: 6-char concatenation → decimal integer (0..999,999)
- **Storage**: CSR (Compressed Sparse Row) with direct addressing

#### DelIndex (Case B: indel=1)
- **Purpose**: Handle 1 insertion + 1 deletion cases
- **Strategy**: Generate 15 variants by deleting each position, split 14-char result into 7+7
- **Implementation**: Single unified index for both left7 and right7 keys
- **Key format**: 7-char substring → decimal integer (0..9,999,999)
- **Storage**: CSR with packed postings (id:20bit | del_pos:4bit)

#### CaseFilterIndex
Main structure combining both indices:
- `keywords[]`: Raw 16-byte strings (continuous array for O(1) ID→string lookup)
- `codes[]`: 4-bit packed 60-bit representations for SWAR Hamming computation
- `hidx`: HIndex for Case A
- `del7`: DelIndex for Case B

### Search Algorithm Flow

1. **Case A (indel=0)**:
   - Partition query into 5 blocks, generate 10 pair keys
   - Sort pairs by posting list length (shortest first for early termination)
   - For each candidate ID (with visited bitmap to avoid duplicates):
     - Compute `hamming_packed15()` using SWAR
     - If ≤3, return 1 (exact match for fixed-length strings)

2. **Case B (indel=1)**:
   - For each deletion position (0..14):
     - Pack deleted 14-char query
     - Look up left7 and right7 keys in DelIndex
     - For each candidate:
       - Pack deleted keyword at stored del_pos
       - Compute `hamming_packed14()`
       - If 2 + hamming14 ≤ 3 (i.e., hamming14 ≤ 1), return 1
   - Note: visited bitmap only set on hits to allow re-evaluation at different del_pos

3. Return 0 if no matches found

### Key Implementation Details

#### SWAR Hamming Distance
- 15 chars × 4 bits = 60 bits fit in single `uint64_t`
- `hamming_packed15()` and `hamming_packed14()` use bit-parallel computation
- Much faster than byte-by-byte comparison

#### Visited Bitmap Optimization
- Static buffer reused across queries
- Generation counter (`gen`) increments instead of clearing bitmap
- Only reallocates on keyword count increase
- `memset` only on counter overflow

#### CSR Direct Addressing
- No bucket linked lists or entry arrays
- `count[key]` → prefix-sum → `offset[key]`
- Postings stored in contiguous `ids[]` or `idpos[]` arrays
- Memory efficient, cache friendly

## Development Guidelines

### Modifying Search Logic
- Case A and Case B are independent and can be optimized separately
- Early termination is critical for performance: sort by posting list length
- visited bitmap logic differs between cases: A marks all evaluations, B marks only hits

### Adding New Distance Thresholds
- Current design is optimized for k=3 and indel ∈ {0,1}
- k>3 would require additional cases (indel=2 gives edit ≥4)
- Extending requires analyzing new indel/substitution combinations

### Memory Optimization
- Currently using 32-bit counts and offsets
- Postings use 3 bytes in serialized form (20-bit IDs)
- Target: fit within 200MB constraint
- Consider bit-packing and CSR compression for larger datasets

### Performance Profiling
- Use `record_perf` utility for systematic measurement
- Compare against naive Levenshtein baseline
- Focus on posting list traversal and Hamming computation hotspots

## Language and Documentation

- Code comments and documentation are primarily in Japanese
- Algorithm documentation: `src/casefilter/ALGORITHM.md`
- README files contain build instructions and usage examples
- Validation script has English docstrings
