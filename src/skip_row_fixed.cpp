// skip_row_fixed.cpp
//
// A FIXED version of CSVFormatReader::skipRow(), for demonstrating that the
// bug identified in skip_row.cpp is real and that the obvious fix resolves
// it. Exactly one line differs from the original (see "FIX:" comment below)
// — everything else is byte-for-byte identical to skip_row.cpp.
//
// The fix: replace the stale local-pointer read `*pos` with `*istr.position()`
// (the buffer's real, current cursor — correctly reflecting any refill that
// `!istr.eof()` may have just triggered).

#include "mini_read_buffer.h"

#include <stdexcept>

namespace
{

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

void skipRowFixed(MiniReadBuffer & istr, bool allow_cr_end_of_line)
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
                // FIX: was `*pos == '\n'` — `pos` is stale after the refill
                // that `!istr.eof()` may have just triggered; `istr.position()`
                // is the buffer's real, current cursor and must be used instead.
                if (!istr.eof() && *istr.position() == '\n')
                {
                    ++istr.position();
                    return;
                }
            }
        }
    }
}
