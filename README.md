# Row-Count-Bug

Minimal, standalone C++ reproduction of a confirmed off-by-one bug in
ClickHouse's CSV row-counting fast path (`optimize_count_from_files`),
discovered while investigating a `+1` row-count mismatch reported by
`check_rowcounts.sh` against a real PMU CSV file ingested via
`azureBlobStorage()`.

**Status: root cause confirmed, fix demonstrated, fully reproducible in a
147-byte synthetic file plus the original real-world file.**

## Background

ClickHouse has a setting `optimize_count_from_files` (default: enabled,
since v23.8) that lets `SELECT count() FROM azureBlobStorage(...)` (and
`file`/`s3`/`url`/`hdfs`) avoid fully parsing every row — for CSV, it does
this by reusing a lightweight, quote-aware row-boundary scanner
(`CSVFormatReader::skipRow()`, invoked via `countRows()`) instead of the
full field-parsing path.

For one specific real-world file, this fast path consistently and
reproducibly returns one row *more* than the file actually contains —
originally confirmed against the live ClickHouse server via:
- `SELECT count()` vs `SELECT count(), uniqExact(Timestamp)` in the same
  query (the latter always agrees with the true row count; only the bare
  `count()` fast path disagrees).
- Disabling the optimization (`optimize_count_from_files=0`) reliably
  restores the correct count.
- Ground-truth inspection of the file itself (byte-for-byte, via `wc -l`,
  Python's `csv` module, and a manual quote-aware newline scan) shows the
  file is completely well-formed — no malformed rows, no duplicate
  timestamps, no embedded/escaped quotes.

## Confirmed root cause

Reading ClickHouse's source
(`src/Processors/Formats/Impl/CSVRowInputFormat.cpp`,
`CSVFormatReader::skipRow()`), the `\r`-handling branch has a stale-pointer
bug:

```cpp
if (*pos == '\r')
{
    ++istr.position();                    // advances the REAL buffer cursor
    if (format_settings.csv.allow_cr_end_of_line)
        return;
    if (!istr.eof() && *pos == '\n')       // BUG: reads *pos — a local variable
    {                                       // that was never advanced, not
        ++pos;                             // *istr.position() (the real cursor)
        return;
    }
}
```

`pos` is a plain local `char*` (from `find_first_symbols<...>(...)`), while
`istr.position()` returns a **reference** to the buffer's own internal
cursor (`Position & position() { return pos; }` in `IO/BufferBase.h`,
confirmed by reading the actual declarations — `using Position = char *;`).
`pos` and `istr.position()` are two independent variables that happen to
hold equal values right up until `++istr.position()` executes — after that,
`pos` is stale.

In the common case this is harmless (the stale check fails, the outer loop
retries and finds the row ending correctly on the next pass). But the
`!istr.eof()` call on the line above can trigger an internal buffer refill
if `\r` happens to be the very last byte of the currently loaded chunk. If
the refill reuses the same memory block (as a performance-oriented
streaming buffer typically would), reading `*pos` becomes a stale read of
memory that has just been overwritten with new content — and if that new
content coincidentally has `'\n'` at the same offset, `skipRow()` returns
early *without* having actually consumed the real `\n` (now the first byte
of the newly refilled chunk). That leftover, unconsumed `\n` then gets
counted as a phantom empty row on the very next call to `skipRow()`.

**Two independent coincidences are required for this to manifest:**
1. `\r` must be the last byte of the currently loaded chunk (a `DBMS_DEFAULT_BUFFER_SIZE`
   = 1,048,576-byte boundary in production — confirmed via
   `ZlibInflatingReadBuffer::nextImpl()`, which requests output from zlib in
   chunks of exactly that size by default).
2. Whatever the **next** refill places at that *same, reused* buffer offset
   must itself happen to be `'\n'`.

Both conditions are fixed, deterministic properties of a given file's exact
byte content — explaining why the bug reproduces 100% of the time for one
specific file and (as observed) essentially never for any other file in the
same dataset.

## Empirical confirmation (all reproduced in this repo — see `tests/`)

1. **Real production file** (`.gz`, referenced via `ROWCOUNT_TEST_FILE`, never
   committed): the original `skipRow()` gives **15001** rows under the
   `InPlaceReuse` refill strategy (matching the exact production bug) and
   **15000** (correct) under `FreeAndReallocate`.
2. **AddressSanitizer** confirms a genuine `heap-use-after-free` at
   **exactly** the predicted line (the `*pos == '\n'` check) and **exactly**
   the predicted byte offset (the buffer's last byte — where `\r` was)
   under `FreeAndReallocate`.
3. **Fixed version** (`skipRowFixed()`, one-line change: `*pos` →
   `*istr.position()`) gives the correct **15000** under *both* refill
   strategies, and produces **no** ASAN finding at all.
4. **Minimal synthetic repro** (`tests/fixtures/boundary_1048576.csv[.gz]`,
   ~2 MB, `buf_size=1048576` — matching `DBMS_DEFAULT_BUFFER_SIZE`, i.e. the
   real ClickHouse default — containing no real data; generated on demand,
   not committed, see "Data handling" below): engineered so both
   coincidences above hold. Reproduces the exact same bug class end-to-end:
   buggy+`InPlaceReuse` → 110169 rows (should be 110168); ASAN confirms
   `heap-use-after-free`, "1048575 bytes inside of 1048576-byte region"
   under buggy+`FreeAndReallocate`; fixed version → correct (110168) under
   both strategies, no ASAN finding.
5. **Negative control** (`tests/fixtures/control_1048576.csv[.gz]`): same
   structure, CRLF shifted one byte so it sits safely within a single
   chunk (no boundary coincidence) — buggy code gives the correct count
   (55084/55084), proving the bug is specifically about the boundary
   coincidence, not just "any CRLF-terminated CSV".
6. **Live ClickHouse server** (version 26.3.32.14): the same
   `boundary_1048576.csv[.gz]` / `control_1048576.csv[.gz]` fixtures,
   copied into `user_files` and queried via
   `SELECT count() FROM file(...) SETTINGS optimize_count_from_files = 1`
   with **default settings** (no `max_read_buffer_size` override needed —
   1,048,576 already matches the real default), reproduce exactly the same
   +1 over-count on `boundary_1048576` (110169 vs. ground-truth 110168) and
   the correct count on `control_1048576` (55084), for both the plain and
   gzip-compressed variants. This confirms the bug (and its fixture) is not
   an artifact of this standalone harness — it reproduces identically
   against a real, unmodified ClickHouse server.

An earlier, smaller synthetic fixture (`buf_size=64`, ~150 bytes) reproduced
the bug in this standalone harness but did **not** reproduce against a live
server at that scale: `max_read_buffer_size` only resizes the raw
compressed/file-level read buffer, not the zlib decompression *output*
buffer (which is hardcoded to `DBMS_DEFAULT_BUFFER_SIZE` via
`wrapReadBufferWithCompressionMethod`'s default argument), and even the
plain (uncompressed) case didn't reproduce at that tiny scale — most likely
because `CSVFormatReader::skipRow()`'s `istr` is actually a
`PeekableReadBuffer` wrapping the raw buffer, not the raw buffer itself,
and its own buffering behavior differs from the raw buffer's own
chunk-boundary alignment at very small sizes. Matching the real default
buffer size sidesteps this entirely and was sufficient to reproduce live.

## Approach (as executed)

- **Phase 1**: built the harness with **real zlib decompression**
  (mirroring `ZlibInflatingReadBuffer::nextImpl()` exactly: one
  `inflate(Z_NO_FLUSH)` call per refill, output capped at a configurable
  `buf_size`), ran against the real production `.gz` file under both
  refill strategies. **Result: bug reproduced under `InPlaceReuse`; ASAN
  confirmed a genuine use-after-free under `FreeAndReallocate`.**
- **Phase 1.5**: added `skipRowFixed()` — the same logic with the one-line
  fix — and ran it against the same real file under both strategies.
  **Result: correct count (15000) in all cases, no ASAN finding.**
- **Phase 2**: engineered small (147-byte), purely-synthetic fixture files
  reproducing both required coincidences at a much smaller `buf_size` (64,
  vs. the real 1,048,576), plus a negative control.
  **Result: the synthetic files reproduce the exact same bug class (and
  its fix) as the real file in this standalone harness — but, see Phase 3,
  did not reproduce against a live server at that tiny scale.**
  (First attempt at a synthetic file only engineered coincidence #1 and
  did *not* reproduce the bug — see git history / script comments for why:
  the second refill was too short to overwrite the stale byte at all. This
  is itself informative and is documented in
  `tests/fixtures/generate_test_files.py`.)
- **Phase 3**: regenerated the synthetic fixtures at the real
  `buf_size=1048576` (`DBMS_DEFAULT_BUFFER_SIZE`) instead of the toy 64-byte
  scale, and ran the resulting `count()` queries against a live ClickHouse
  server (version 26.3.32.14) via `file('boundary_1048576.csv[.gz]', ...)`.
  **Result: reproduces live, with default settings, for both plain and
  gzip-compressed CSV — confirming the bug (and this fixture) live outside
  the standalone harness.**

## Project rule: no unproven simplifications

Reproduce the real situation as faithfully as possible. Do not simplify,
mock, or skip any part of the real pipeline (decompression, buffer sizing,
refill strategy, etc.) unless it has first been empirically proven
irrelevant to the bug. (This is why Phase 2's synthetic files still go
through real zlib decompression, just with a smaller `buf_size` — only the
buffer size was ever a free parameter in the theory, not the decompression
mechanism itself.)

## Data handling

**Real production data (Statnett PMU files) is never committed to this
repository.** The test harness reads the real file's path from the
`ROWCOUNT_TEST_FILE` environment variable at test-run time; if unset, the
corresponding test cases are skipped (not failed), so the suite still runs
cleanly for anyone without access to the real file. Optionally set
`ROWCOUNT_EXPECTED_DATA_ROWS` to assert the count against a known value.

`.gitignore` excludes `*.csv`/`*.csv.gz`/`*.gz`/`data/` by default. The
generated synthetic fixtures under `tests/fixtures/` are **not committed**
(regenerated on demand via `make fixtures` / `generate_test_files.py`,
since the real-scale ones are ~2 MB) — only the generator script itself is
tracked in git.

## Project layout

```
Row-Count-Bug/
├── README.md
├── .gitignore
├── third_party/
│   └── doctest.h              — vendored single-header test framework
├── src/
│   ├── mini_read_buffer.h     — minimal ReadBuffer-alike + real zlib decompression
│   │                            (two refill strategies: InPlaceReuse, FreeAndReallocate)
│   ├── skip_row.cpp           — CSVFormatReader::skipRow(), copied verbatim (buggy)
│   ├── skip_row_fixed.cpp     — same, with the one-line fix
│   ├── skip_row.h             — declarations for both + countRows()
│   └── count_rows.cpp         — countRows() driver loop, copied/adapted,
│                                 parametrized over which skipRow variant to use
├── tests/
│   ├── test_row_count.cpp     — doctest cases: real file (buggy/fixed x 2
│   │                            strategies) + synthetic fixtures (boundary/
│   │                            control x buggy/fixed x 2 strategies)
│   └── fixtures/
│       └── generate_test_files.py  — builds the synthetic repro/control
│                                      files (boundary_<N>.csv[.gz],
│                                      control_<N>.csv[.gz] — not committed,
│                                      see "Data handling")
└── Makefile                   — `make fixtures`, `make test`, `make test-asan`
```

## Usage

```bash
# Against the real file (optional — tests are skipped if unset):
export ROWCOUNT_TEST_FILE=/absolute/path/to/real_file.csv.gz
export ROWCOUNT_EXPECTED_DATA_ROWS=15000

make test        # regenerates fixtures (via `make fixtures`) if missing,
                  # then builds+runs all test cases
make test-asan   # same tests under -fsanitize=address,undefined

# To isolate buggy vs. fixed (the combined ASAN binary aborts on the first
# use-after-free it hits, so run these separately to see both results):
./build/test_row_count_asan --test-case="*[buggy]*"
./build/test_row_count_asan --test-case="*[fixed]*"

# To (re)generate the synthetic fixtures manually, or build new ones at a
# different buf_size:
python3 tests/fixtures/generate_test_files.py 1048576 tests/fixtures
```

## Suggested next steps

- Post the upstream ClickHouse pull request (draft prepared, includes the
  live-server-confirmed repro steps using `generate_test_files.py 1048576`).
- `check_rowcounts.sh` (in the `Scripts` repo) was already updated with
  `optimize_count_from_files=0` before this root cause was fully confirmed
  — that fix is validated by everything in this repo.
