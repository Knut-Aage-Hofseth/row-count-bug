# Row-Count-Bug

Minimal, standalone C++ reproduction of a suspected off-by-one bug in
ClickHouse's CSV row-counting fast path (`optimize_count_from_files`),
discovered while investigating a `+1` row-count mismatch reported by
`check_rowcounts.sh` against a real PMU CSV file ingested via
`azureBlobStorage()`.

## Background

ClickHouse has a setting `optimize_count_from_files` (default: enabled,
since v23.8) that lets `SELECT count() FROM azureBlobStorage(...)` (and
`file`/`s3`/`url`/`hdfs`) avoid fully parsing every row — for CSV, it does
this by reusing a lightweight, quote-aware row-boundary scanner
(`CSVFormatReader::skipRow()`, invoked via `countRows()`) instead of the
full field-parsing path.

For one specific real-world file, this fast path consistently and
reproducibly returns one row *more* than the file actually contains —
confirmed via:
- `SELECT count()` vs `SELECT count(), uniqExact(Timestamp)` in the same
  query (the latter always agrees with the true row count; only the bare
  `count()` fast path disagrees).
- Disabling the optimization (`optimize_count_from_files=0`) reliably
  restores the correct count.
- Ground-truth inspection of the file itself (byte-for-byte, via `wc -l`,
  Python's `csv` module, and a manual quote-aware newline scan) shows the
  file is completely well-formed — no malformed rows, no duplicate
  timestamps, no embedded/escaped quotes.

## Suspected root cause

Reading ClickHouse's source
(`src/Processors/Formats/Impl/CSVRowInputFormat.cpp`,
`CSVFormatReader::skipRow()`), the `\r`-handling branch has what looks
like a stale-pointer bug:

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
cursor (`Position & position() { return pos; }` in `IO/BufferBase.h`).
They are two independent variables that happen to hold equal values right
up until `++istr.position()` executes — after that, `pos` is stale.

In the common case this is harmless (the stale check fails, the outer loop
retries and finds the row ending correctly on the next pass). But the
`!istr.eof()` call on the line above can trigger an internal buffer refill
if `\r` happens to be the very last byte of the currently loaded chunk. If
the refill reuses/invalidates the old buffer memory, reading `*pos`
becomes a stale/out-of-date read that can coincidentally equal `'\n'` —
causing `skipRow()` to return early *without* having actually consumed the
real `\n` (which is now the first byte of the newly refilled chunk). That
leftover, unconsumed `\n` then gets counted as a phantom empty row on the
very next call to `skipRow()`.

This exactly matches the real file's byte layout: it has a `\r\n` line
ending split precisely across a `DBMS_DEFAULT_BUFFER_SIZE` (1,048,576-byte)
boundary — confirmed via `ZlibInflatingReadBuffer::nextImpl()`, which
requests output from zlib in chunks of exactly that size by default.

## Goal of this project

Prove or disprove this mechanism with a minimal, standalone C++ test rig —
copying only the exact `skipRow()` logic (verbatim) plus a from-scratch
minimal buffer class that reproduces just enough of ClickHouse's
`ReadBuffer` contract (`position()` returning a reference, `buffer().end()`,
`eof()`-triggers-refill) to exercise the bug, without needing to build all
of ClickHouse or link its full dependency chain (`fmt`, `Poco`, etc.).

## Approach

1. **Phase 1 (current)**: build the harness with **real zlib decompression**
   (mirroring `ZlibInflatingReadBuffer::nextImpl()` exactly: one
   `inflate(Z_NO_FLUSH)` call per refill, output capped at a configurable
   `buf_size`, defaulting to 1,048,576 to match ClickHouse), run against
   the real production `.gz` file, and see whether the `+1` reproduces
   under two buffer-refill strategies:
   - in-place reuse (overwrite the same memory block on refill)
   - free + reallocate (new block each refill), built additionally under
     AddressSanitizer to check for a tool-confirmed memory-safety issue
2. **Phase 2 (only after Phase 1 confirms the rig behaves as expected)**:
   validate whether a simpler flat-file, fixed-size-chunking harness (no
   real gzip decompression) behaves identically to Phase 1 for the same
   file. Only if proven equivalent will simpler synthetic (uncompressed)
   test files be used for isolated/control repro cases going forward —
   per the project rule below.

## Project rule: no unproven simplifications

Reproduce the real situation as faithfully as possible. Do not simplify,
mock, or skip any part of the real pipeline (decompression, buffer sizing,
refill strategy, etc.) unless it has first been empirically proven
irrelevant to the bug.

## Data handling

**Real production data (Statnett PMU files) is never committed to this
repository.** The test harness reads the real file's path from the
`ROWCOUNT_TEST_FILE` environment variable at test-run time; if unset, the
corresponding test case is skipped (not failed), so the suite still runs
cleanly for anyone without access to the real file. `.gitignore` excludes
`*.csv`/`*.csv.gz`/`data/`/`fixtures/` as a safety net.

Synthetic test files (containing only engineered filler content, no real
station data) are safe to commit once Phase 2 is reached.

## Project layout

```
Row-Count-Bug/
├── README.md
├── .gitignore
├── third_party/
│   └── doctest.h              — vendored single-header test framework
├── src/
│   ├── mini_read_buffer.h     — minimal ReadBuffer-alike + real zlib decompression
│   ├── skip_row.cpp           — CSVFormatReader::skipRow(), copied verbatim
│   └── count_rows.cpp         — countRows() driver loop, copied/adapted
├── tests/
│   ├── test_row_count.cpp     — doctest cases (reads ROWCOUNT_TEST_FILE)
│   └── fixtures/              — synthetic test files (added in Phase 2)
└── Makefile                   — `make test`, `make test-asan`
```

## Usage (once built)

```bash
export ROWCOUNT_TEST_FILE=/absolute/path/to/real_file.csv.gz
make test        # normal build, both refill strategies
make test-asan   # same tests under -fsanitize=address,undefined
```

## Status

Project scaffolding in progress. See commit history / issues for current
state.
