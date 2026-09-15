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
size_t countDataRows(const std::string & path, size_t buf_size, ZlibMiniReadBuffer::RefillStrategy strategy)
{
    ZlibMiniReadBuffer buf(path, buf_size, strategy);

    // Skip the header row.
    skipRow(buf, /*allow_cr_end_of_line=*/false);

    // Count every remaining row. max_block_size is effectively "unbounded"
    // for our purposes (real files here are a few million rows at most).
    return countRows(buf, /*max_block_size=*/static_cast<size_t>(-1), /*allow_cr_end_of_line=*/false);
}

constexpr size_t kDefaultBufSize = 1048576; // DBMS_DEFAULT_BUFFER_SIZE

} // namespace

TEST_CASE("real file: in-place-reuse refill strategy")
{
    auto path = getEnvVar("ROWCOUNT_TEST_FILE");
    if (!path)
    {
        MESSAGE("ROWCOUNT_TEST_FILE not set — skipping (see README.md)");
        return;
    }

    size_t rows = countDataRows(*path, kDefaultBufSize, ZlibMiniReadBuffer::RefillStrategy::InPlaceReuse);
    MESSAGE("InPlaceReuse: counted ", rows, " data rows");

    if (auto expected = getEnvVar("ROWCOUNT_EXPECTED_DATA_ROWS"))
        CHECK(rows == static_cast<size_t>(std::stoull(*expected)));
}

TEST_CASE("real file: free-and-reallocate refill strategy")
{
    auto path = getEnvVar("ROWCOUNT_TEST_FILE");
    if (!path)
    {
        MESSAGE("ROWCOUNT_TEST_FILE not set — skipping (see README.md)");
        return;
    }

    size_t rows = countDataRows(*path, kDefaultBufSize, ZlibMiniReadBuffer::RefillStrategy::FreeAndReallocate);
    MESSAGE("FreeAndReallocate: counted ", rows, " data rows");

    if (auto expected = getEnvVar("ROWCOUNT_EXPECTED_DATA_ROWS"))
        CHECK(rows == static_cast<size_t>(std::stoull(*expected)));
}
