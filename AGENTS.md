# Repository Guidelines

## Project Structure & Module Organization
- Core binaries are monolithic C files: `prep_casefilter.c` builds the index and `search_casefilter.c` runs queries; compiled outputs `prep_casefilter` and `search_casefilter` live in the repo root. `record_perf.c`/`record_perf` capture performance metrics.
- `casefilter_instructions/` holds the split reference version (`index.h`, `build.c`, `search.c`) plus design notes; use it for deeper algorithm context but build from the monolithic sources.
- `test-data/` contains fixed-length (15 chars, A–J) sample databases and queries (`db_*`, `query_*`). `output/` is for generated indexes/results (e.g., `output/index_casefilter_1`), and `records/` stores CSV performance logs.
- `validate.py` is a Python validator that cross-checks the C binaries against a naive Levenshtein implementation. `record_perf_test.sh` sanity-checks the performance logger.

## Build, Test, and Development Commands
- Build binaries (no external deps): `gcc -O2 prep_casefilter.c -o prep_casefilter`, `gcc -O2 search_casefilter.c -o search_casefilter`, `gcc -O2 record_perf.c -o record_perf`.
- Prepare an index and run a small query set: `./prep_casefilter test-data/db_1 > output/index_casefilter_1` then `./search_casefilter test-data/query_1 output/index_casefilter_1 > output/result_casefilter`.
- Validate correctness: `python3 validate.py --prep-bin ./prep_casefilter --search-bin ./search_casefilter --db test-data/db_1 --query test-data/query_1` (add `--index output/index_casefilter_1` to reuse an existing index).
- Profile or log performance: `/usr/bin/time -f 'search %e' ./search_casefilter test-data/query_100k output/index_casefilter_100k > /dev/null` or `./record_perf --record -- ./search_casefilter …`.
- Smoke test the logger after building `record_perf`: `./record_perf_test.sh` (writes temp CSVs, cleans up automatically).

## Coding Style & Naming Conventions
- Language is C11-friendly; use 4-space indentation, no tabs, and keep lines focused. Favor `snake_case` for functions/variables, `SCREAMING_SNAKE_CASE` for macros/constants.
- Keep helper functions `static` when file-local; prefer explicit integer widths (`uint32_t`, `int`) and preallocated buffers to avoid heap churn.
- Comments may mix concise Japanese/English; explain non-obvious bit packing or indexing math, not trivial steps. Preserve existing buffer sizes and key-spacing constants.
- I/O must check return values (`fread`/`fwrite`), and allocations should be validated before use.

## Testing Guidelines
- Minimal functional check: build both binaries and run `validate.py` against a small dataset (`db_1`/`query_1`). For larger datasets, reuse existing indexes to save time.
- When changing serialization or memory layouts, regenerate an index and rerun `validate.py` plus a timed query run; archive any new `records/perf_*.csv` in `records/`.
- If you modify `record_perf`, run `./record_perf_test.sh`; failures usually mean path assumptions changed.

## Commit & Pull Request Guidelines
- Use short, imperative commit titles (e.g., “Tighten hamming filter bounds”) with optional bullet details for data structures, performance impact, and test commands run.
- PRs should mention the dataset/query size used for validation, include the exact commands executed, and summarize any performance changes (attach `records/perf_*.csv` snippets if relevant).
- Avoid committing bulky generated artifacts unless intentionally sharing a baseline (large indexes/CSV logs can be regenerated).

## Security & Configuration Tips
- Inputs are untrusted; maintain bounds checks on offsets and counts when deserializing indexes. Avoid introducing dynamic allocations inside tight loops without size guards.
- Keep test and output paths relative to the repo root to simplify repro and avoid writing outside the workspace.
