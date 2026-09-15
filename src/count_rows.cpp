// count_rows.cpp
//
// Adapted from RowInputFormatWithNamesAndTypes<FormatReaderImpl>::countRows()
// (src/Processors/Formats/RowInputFormatWithNamesAndTypes.cpp, lines ~327-345
// in the cloned repo) and CSVFormatReader::checkForSuffix()/
// skipRowBetweenDelimiter() (src/Processors/Formats/Impl/CSVRowInputFormat.cpp).
//
// Real loop:
//   while (!format_reader->checkForSuffix() && num_rows < max_block_size)
//   {
//       if (likely(!is_first_row))
//           format_reader->skipRowBetweenDelimiter();
//       else
//           is_first_row = false;
//       format_reader->skipRow();
//       ++num_rows;
//   }
//
// Adaptations made (both justified as behaviorally irrelevant to the bug,
// which lives entirely inside skipRow()'s '\r' handling):
//   - CSVFormatReader::checkForSuffix() reduces to exactly `buf->eof()` for the
//     default FormatSettings (`csv.skip_trailing_empty_lines = false`,
//     confirmed in ClickHouse's src/Formats/FormatSettings.h) — that default
//     is what production uses, so we call MiniReadBuffer::eof() directly
//     instead of reimplementing the (dead, for our settings) alternate branch.
//   - CSVFormatReader::skipRowBetweenDelimiter() is not overridden by
//     CSVFormatReader at all — it uses the base class's default no-op
//     (RowInputFormatWithNamesAndTypes.h: `virtual void
//     skipRowBetweenDelimiter() {}`) — so it is simply omitted here rather
//     than reimplemented as an empty call.
//   - This harness always starts counting from the first data row (the
//     header line, if present, must be skipped by the caller before calling
//     countRows() — mirroring how the real IRowInputFormat::read() only
//     invokes countRows() after readPrefix()/header parsing has already
//     consumed the header separately).

#include "skip_row.h"

size_t countRows(MiniReadBuffer & buf, size_t max_block_size, bool allow_cr_end_of_line)
{
    size_t num_rows = 0;
    while (!buf.eof() && num_rows < max_block_size)
    {
        skipRow(buf, allow_cr_end_of_line);
        ++num_rows;
    }
    return num_rows;
}
