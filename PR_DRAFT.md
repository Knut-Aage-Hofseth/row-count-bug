# PR draft (for upstream ClickHouse/ClickHouse)

## Changelog category (leave one):
- Bug Fix (user-visible misbehavior in an official stable release)

## Changelog entry:
Fixes a stale pointer issue in `CSVFormatReader::skipRow()` to fix a possible count off by 1 bug.

---

## Problem

`SELECT count() FROM file('*.csv', 'CSVWithNames')` (and `s3`/`azureBlobStorage`/`url`/`hdfs` equivalents) can return a count that is **1 higher** than the file's actual row count, when `optimize_count_from_files` is enabled (the default since 23.8). This path counts rows via `CSVFormatReader::skipRow()` rather than fully parsing each row.

Originally found via `count()` vs `count(), uniqExact(some_column)` disagreeing on the same real-world file — only the fast-path `count()` was wrong; the file itself is well-formed (independently verified with `wc -l` and Python's `csv` module).

## Root cause

`src/Processors/Formats/Impl/CSVRowInputFormat.cpp`, `CSVFormatReader::skipRow()`:

```cpp
if (*pos == '\r')
{
    ++istr.position();
    if (format_settings.csv.allow_cr_end_of_line)
        return;
    if (!istr.eof() && *pos == '\n')   // <-- BUG: reads stale local `pos`
    {
        ++pos;
        return;
    }
}
```

(currently https://github.com/ClickHouse/ClickHouse/blob/master/src/Processors/Formats/Impl/CSVRowInputFormat.cpp#L173-L183)

`pos` is a local `char *` obtained from `find_first_symbols<...>(...)` a few lines earlier. `istr.position()` returns a **reference** to the buffer's own internal cursor (`Position & position()` in `IO/BufferBase.h`) — a separate variable that only happens to hold the same value as `pos`, right up until `++istr.position()` on the line above advances it.

The `!istr.eof()` call can trigger an internal buffer refill if `\r` is the very last byte of the currently loaded chunk (e.g. exactly at the `DBMS_DEFAULT_BUFFER_SIZE` = 1,048,576-byte boundary for a decompressed stream, or the plain-file read buffer of the same size). After that refill, the memory `pos` still points into may be reused/overwritten with new content (if the buffer is reused in place), or freed and replaced entirely (if the implementation allocates a fresh buffer per refill — confirmed a genuine **heap-use-after-free** in that case, see below).

If, by coincidence, the byte now sitting at that same offset is `'\n'`, `skipRow()` returns **without having consumed the real `\n`**. That leftover `\n` is counted as a phantom empty row on the very next call to `skipRow()` — producing the +1.

Two conditions must both hold for this to manifest:
1. `\r` lands exactly on the last byte of the currently-buffered chunk.
2. The byte at that same buffer offset, after the next refill, is `\n`.

Both are fixed properties of a given file's exact byte content, which is why the bug is 100% reproducible for one specific file/offset and not observed for others.

## Fix

One-line change: read the real, current cursor instead of the stale local variable.

```cpp
if (!istr.eof() && *istr.position() == '\n')
{
    ++istr.position();
    return;
}
```

## Repro steps (confirmed live against a real ClickHouse server)

Generate two small synthetic CSV files (no real data) using the script from https://github.com/<your-username>/Row-Count-Bug (`tests/fixtures/generate_test_files.py`) — see that repo for the full standalone reproduction/root-cause harness, including an AddressSanitizer-confirmed heap-use-after-free trace:

```bash
python3 generate_test_files.py 1048576 .
```

This produces `boundary_1048576.csv[.gz]` (110,168 real data rows, engineered so a row's CRLF line ending straddles the 1,048,576-byte buffer boundary, and the following chunk happens to place a `\n` at the same offset) and `control_1048576.csv[.gz]` (55,084 data rows, same structure but shifted so the CRLF sits safely inside one chunk — negative control).

Copy both pairs of files into ClickHouse's `user_files` directory, then, with **default settings** (no `max_read_buffer_size` override needed — 1,048,576 already matches `DBMS_DEFAULT_BUFFER_SIZE`):

```sql
-- over-counts by 1
SELECT count() FROM file('boundary_1048576.csv', 'CSVWithNames') SETTINGS optimize_count_from_files = 1;
-- => 110169

-- ground truth (fast path disabled)
SELECT count() FROM file('boundary_1048576.csv', 'CSVWithNames') SETTINGS optimize_count_from_files = 0;
-- => 110168

-- negative control — correct even with the fast path, since there's no boundary coincidence
SELECT count() FROM file('control_1048576.csv', 'CSVWithNames') SETTINGS optimize_count_from_files = 1;
-- => 55084

-- same results against the gzip-compressed variants (exercises real decompression):
SELECT count() FROM file('boundary_1048576.csv.gz', 'CSVWithNames') SETTINGS optimize_count_from_files = 1;
-- => 110169
SELECT count() FROM file('boundary_1048576.csv.gz', 'CSVWithNames') SETTINGS optimize_count_from_files = 0;
-- => 110168
SELECT count() FROM file('control_1048576.csv.gz', 'CSVWithNames') SETTINGS optimize_count_from_files = 1;
-- => 55084
```

All six results above were obtained running against a live ClickHouse server (26.3.32.14).

## Standalone minimal reproduction (isolates the bug outside a full ClickHouse build)

A minimal, standalone C++ harness (copies `skipRow()`'s exact logic, `IO/BufferBase.h`'s `position()` contract, and mirrors `ZlibInflatingReadBuffer::nextImpl()`'s refill behavior) is available at https://github.com/<your-username>/Row-Count-Bug. Under a "free-and-reallocate-per-refill" buffer strategy, AddressSanitizer flags a `heap-use-after-free` at exactly this line, at exactly the last byte of the buffer:

```
==NNNN==ERROR: AddressSanitizer: heap-use-after-free ...
READ of size 1 at 0x...
    #0 skipRow(...) CSVRowInputFormat.cpp:178 (skip_row.cpp:109 in the standalone harness)
...
0x... is located 1048575 bytes inside of 1048576-byte region [...]
freed by thread T0 here:
    ... (buffer refill / reallocation)
```

The harness also demonstrates the one-line fix resolves both the miscount and the use-after-free, under both a "reuse buffer in place" and a "free and reallocate" refill strategy, against both the synthetic fixtures above and the original real-world file that surfaced this bug.

---

### Notes for whoever posts this
- Update `<your-username>` with the actual GitHub repo URL before posting.
- Line numbers (`CSVRowInputFormat.cpp#L173-L183`) reflect the current `master` at the time of writing — worth a quick diff check against `master` right before submitting, in case the file has moved.
- `generate_test_files.py` is attached/linked rather than the generated files themselves, since the files are large-ish (~2 MB plain / ~270 KB gzip for `boundary_1048576`) and trivially regenerable.
