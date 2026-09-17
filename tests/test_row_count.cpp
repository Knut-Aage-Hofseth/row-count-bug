// test_row_count.cpp
//
// Phase 1 tests: run the copied CSVFormatReader::skipRow()/countRows() logic
// against the REAL production .gz file (never committed to this repo — see
// README.md and .gitignore) through REAL zlib decompression, under both
// buffer-refill strategies, and report the counted rows.
//
// The real file's path is read from the ROWCOUNT_TEST_FILE environment
// variable. If unset, these tests are SKIPPED (not failed), so the suite
// still runs cleanly for anyone without access to the real file.
//
// If ROWCOUNT_EXPECTED_DATA_ROWS is also set, the counted data row count
// (i.e. total rows minus the one header row) is asserted against it.
// Otherwise the count is just reported via MESSAGE() for manual inspection.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../third_party/doctest.h"

#include <cstdlib>
#include <optional>
#include <string>

#include "../src/mini_read_buffer.h"
#include "../src/skip_row.h"

namespace
{

std::optional<std::string> getEnvVar(const char * name)
{
    const char * value = std::getenv(name);
    if (!value || std::string(value).empty())
        return std::nullopt;
    return std::string(value);
}

/// Runs the real countRows() logic against the given file, having first
/// skipped exactly one header row (mirroring how the real pipeline consumes
/// the CSV header separately, before countRows() ever runs).
size_t countDataRows(
    const std::string & path, size_t buf_size, ZlibMiniReadBuffer::RefillStrategy strategy, SkipRowFn skip_row_fn)
{
    ZlibMiniReadBuffer buf(path, buf_size, strategy);

    // Skip the header row.
    skip_row_fn(buf, /*allow_cr_end_of_line=*/false);

    // Count every remaining row. max_block_size is effectively "unbounded"
    // for our purposes (real files here are a few million rows at most).
    return countRows(buf, /*max_block_size=*/static_cast<size_t>(-1), /*allow_cr_end_of_line=*/false, skip_row_fn);
}

constexpr size_t kDefaultBufSize = 1048576; // DBMS_DEFAULT_BUFFER_SIZE

} // namespace

// ---------------------------------------------------------------------------
// Phase 1: the original (buggy) skipRow() against the real production file.
// ---------------------------------------------------------------------------

TEST_CASE("[buggy] real file: in-place-reuse refill strategy")
{
    auto path = getEnvVar("ROWCOUNT_TEST_FILE");
    if (!path)
    {
        MESSAGE("ROWCOUNT_TEST_FILE not set — skipping (see README.md)");
        return;
    }

    size_t rows = countDataRows(*path, kDefaultBufSize, ZlibMiniReadBuffer::RefillStrategy::InPlaceReuse, skipRow);
    MESSAGE("[buggy] InPlaceReuse: counted ", rows, " data rows");

    if (auto expected = getEnvVar("ROWCOUNT_EXPECTED_DATA_ROWS"))
        CHECK(rows == static_cast<size_t>(std::stoull(*expected)));
}

TEST_CASE("[buggy] real file: free-and-reallocate refill strategy")
{
    auto path = getEnvVar("ROWCOUNT_TEST_FILE");
    if (!path)
    {
        MESSAGE("ROWCOUNT_TEST_FILE not set — skipping (see README.md)");
        return;
    }

    size_t rows = countDataRows(*path, kDefaultBufSize, ZlibMiniReadBuffer::RefillStrategy::FreeAndReallocate, skipRow);
    MESSAGE("[buggy] FreeAndReallocate: counted ", rows, " data rows");

    if (auto expected = getEnvVar("ROWCOUNT_EXPECTED_DATA_ROWS"))
        CHECK(rows == static_cast<size_t>(std::stoull(*expected)));
}

// ---------------------------------------------------------------------------
// Phase 1.5: the FIXED skipRowFixed() against the same real file, same
// strategies — demonstrating the fix resolves the bug in both cases.
// ---------------------------------------------------------------------------

TEST_CASE("[fixed] real file: in-place-reuse refill strategy")
{
    auto path = getEnvVar("ROWCOUNT_TEST_FILE");
    if (!path)
    {
        MESSAGE("ROWCOUNT_TEST_FILE not set — skipping (see README.md)");
        return;
    }

    size_t rows
        = countDataRows(*path, kDefaultBufSize, ZlibMiniReadBuffer::RefillStrategy::InPlaceReuse, skipRowFixed);
    MESSAGE("[fixed] InPlaceReuse: counted ", rows, " data rows");

    if (auto expected = getEnvVar("ROWCOUNT_EXPECTED_DATA_ROWS"))
        CHECK(rows == static_cast<size_t>(std::stoull(*expected)));
}

TEST_CASE("[fixed] real file: free-and-reallocate refill strategy")
{
    auto path = getEnvVar("ROWCOUNT_TEST_FILE");
    if (!path)
    {
        MESSAGE("ROWCOUNT_TEST_FILE not set — skipping (see README.md)");
        return;
    }

    size_t rows
        = countDataRows(*path, kDefaultBufSize, ZlibMiniReadBuffer::RefillStrategy::FreeAndReallocate, skipRowFixed);
    MESSAGE("[fixed] FreeAndReallocate: counted ", rows, " data rows");

    if (auto expected = getEnvVar("ROWCOUNT_EXPECTED_DATA_ROWS"))
        CHECK(rows == static_cast<size_t>(std::stoull(*expected)));
}

// ---------------------------------------------------------------------------
// Phase 2: purely-synthetic fixtures generated on demand by
// tests/fixtures/generate_test_files.py at buf_size=1048576 (i.e.
// DBMS_DEFAULT_BUFFER_SIZE, the real ClickHouse default) — no real data
// involved, always run (never skipped). Not committed to git (see
// .gitignore); regenerated by `make fixtures` (a prerequisite of `make
// test`/`make test-asan`) since they're large (~2 MB) and trivially
// reproducible. "boundary" engineers a CRLF split exactly across the
// buf_size boundary (the bug-triggering condition); "control" is the same
// structure shifted so the CRLF sits safely within one chunk, proving the
// bug is specifically about the boundary coincidence. This scale was also
// confirmed to reproduce live against a real ClickHouse server (26.3.32.14)
// via `SELECT count() FROM file(...)`, with default settings (no
// max_read_buffer_size override needed).
// Ground truth for both fixtures: see generator script output
// (boundary_1048576: 110168 data rows; control_1048576: 55084 data rows).
// ---------------------------------------------------------------------------

constexpr size_t kSyntheticBufSize = 1048576;
constexpr size_t kSyntheticBoundaryExpectedRows = 110168;
constexpr size_t kSyntheticControlExpectedRows = 55084;

TEST_CASE("[buggy] synthetic boundary file: in-place-reuse (expect off-by-one)")
{
    size_t rows = countDataRows(
        "tests/fixtures/boundary_1048576.csv.gz", kSyntheticBufSize, ZlibMiniReadBuffer::RefillStrategy::InPlaceReuse, skipRow);
    MESSAGE("[buggy] boundary+InPlaceReuse: counted ", rows, " data rows (expected ", kSyntheticBoundaryExpectedRows, ")");
    CHECK(rows == kSyntheticBoundaryExpectedRows + 1); // documents the reproduced bug
}

TEST_CASE("[buggy] synthetic control file: in-place-reuse (expect correct)")
{
    size_t rows = countDataRows(
        "tests/fixtures/control_1048576.csv.gz", kSyntheticBufSize, ZlibMiniReadBuffer::RefillStrategy::InPlaceReuse, skipRow);
    MESSAGE("[buggy] control+InPlaceReuse: counted ", rows, " data rows (expected ", kSyntheticControlExpectedRows, ")");
    CHECK(rows == kSyntheticControlExpectedRows); // no boundary coincidence -> no bug, even with buggy code
}

TEST_CASE("[fixed] synthetic boundary file: in-place-reuse (expect correct)")
{
    size_t rows = countDataRows(
        "tests/fixtures/boundary_1048576.csv.gz", kSyntheticBufSize, ZlibMiniReadBuffer::RefillStrategy::InPlaceReuse, skipRowFixed);
    MESSAGE("[fixed] boundary+InPlaceReuse: counted ", rows, " data rows (expected ", kSyntheticBoundaryExpectedRows, ")");
    CHECK(rows == kSyntheticBoundaryExpectedRows); // fix resolves the bug even at the boundary
}

TEST_CASE("[buggy] synthetic boundary file: free-and-reallocate (expect correct value, but UAF under ASAN)")
{
    size_t rows = countDataRows(
        "tests/fixtures/boundary_1048576.csv.gz", kSyntheticBufSize, ZlibMiniReadBuffer::RefillStrategy::FreeAndReallocate, skipRow);
    MESSAGE("[buggy] boundary+FreeAndReallocate: counted ", rows, " data rows (expected ", kSyntheticBoundaryExpectedRows, ")");
    CHECK(rows == kSyntheticBoundaryExpectedRows);
}

TEST_CASE("[fixed] synthetic boundary file: free-and-reallocate (expect correct, no UAF)")
{
    size_t rows = countDataRows(
        "tests/fixtures/boundary_1048576.csv.gz", kSyntheticBufSize, ZlibMiniReadBuffer::RefillStrategy::FreeAndReallocate, skipRowFixed);
    MESSAGE("[fixed] boundary+FreeAndReallocate: counted ", rows, " data rows (expected ", kSyntheticBoundaryExpectedRows, ")");
    CHECK(rows == kSyntheticBoundaryExpectedRows);
}



