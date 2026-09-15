// skip_row.cpp
//
// CSVFormatReader::skipRow() copied VERBATIM from ClickHouse's
// src/Processors/Formats/Impl/CSVRowInputFormat.cpp (lines ~124-186 in the
// cloned repo at ~/Prosjekter/ClickHouse), with only the following mechanical
// adaptations (no logic changes):
//   - `ReadBuffer` -> `MiniReadBuffer`
//   - `find_first_symbols<'"', X, Y>(...)` -> `find_first_symbols(...)` calling
//     our trivial reimplementation below (a plain linear scan for any of a set
//     of chars — this utility itself is not implicated in the bug, and its
//     real ClickHouse version is a SIMD-optimized but behaviorally identical
//     search)
//   - `format_settings.csv.allow_cr_end_of_line` -> a plain bool parameter
//   - the two `throw Exception(ErrorCodes::LOGICAL_ERROR, ...)` sanity checks
//     -> `throw std::logic_error(...)` (never expected to trigger; avoids
//     needing ClickHouse's Exception/ErrorCodes machinery)
//
// Everything else — every line of actual control flow, every pointer/reference
// operation — is unchanged from the original.

#include "mini_read_buffer.h"

#include <stdexcept>

namespace
{

/// Trivial reimplementation of ClickHouse's find_first_symbols<Symbols...>.
/// Real implementation: src/Common/find_symbols.h (SIMD-optimized, same
/// semantics: return a pointer to the first occurrence of any of the given
/// chars in [begin, end), or `end` if none found).
MiniReadBuffer::Position find_first_symbols_quote_cr_lf(MiniReadBuffer::Position begin, MiniReadBuffer::Position end)
{
    for (auto * p = begin; p < end; ++p)
    {
        if (*p == '"' || *p == '\r' || *p == '\n')
            return p;
    }
    return end;
}

MiniReadBuffer::Position find_first_symbols_quote(MiniReadBuffer::Position begin, MiniReadBuffer::Position end)
{
    for (auto * p = begin; p < end; ++p)
    {
        if (*p == '"')
            return p;
    }
    return end;
}

} // namespace

/// Copied verbatim from CSVFormatReader::skipRow() — see file header comment
/// for the exact, minimal, purely-mechanical adaptations made.
void skipRow(MiniReadBuffer & istr, bool allow_cr_end_of_line)
{
    bool quotes = false;

    while (!istr.eof())
    {
        if (quotes)
        {
            auto * pos = find_first_symbols_quote(istr.position(), istr.buffer().end());
            istr.position() = pos;

            if (pos > istr.buffer().end())
                throw std::logic_error("Position in buffer is out of bounds. There must be a bug.");
            if (pos == istr.buffer().end())
                continue;
            if (*pos == '"')
            {
                ++istr.position();
                if (!istr.eof() && *istr.position() == '"')
                    ++istr.position();
                else
                    quotes = false;
            }
        }
        else
        {
            auto * pos = find_first_symbols_quote_cr_lf(istr.position(), istr.buffer().end());
            istr.position() = pos;

            if (pos > istr.buffer().end())
                throw std::logic_error("Position in buffer is out of bounds. There must be a bug.");
            if (pos == istr.buffer().end())
                continue;

            if (*pos == '"')
            {
                quotes = true;
                ++istr.position();
                continue;
            }

            if (*pos == '\n')
            {
                ++istr.position();
                if (!istr.eof() && *istr.position() == '\r')
                    ++istr.position();
                return;
            }
            if (*pos == '\r')
            {
                ++istr.position();
                if (allow_cr_end_of_line)
                    return;
                if (!istr.eof() && *pos == '\n')
                {
                    ++pos;
                    return;
                }
            }
        }
    }
}
