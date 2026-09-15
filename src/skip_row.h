// skip_row.h
//
// Declarations for the verbatim-copied skipRow() (see skip_row.cpp for the
// exact provenance/adaptation notes), the FIXED skipRowFixed() (see
// skip_row_fixed.cpp), and the countRows() driver loop.

#pragma once

#include "mini_read_buffer.h"

/// CSVFormatReader::skipRow(), copied verbatim (contains the bug) — see skip_row.cpp.
void skipRow(MiniReadBuffer & istr, bool allow_cr_end_of_line);

/// Fixed version of the above (one-line fix) — see skip_row_fixed.cpp.
void skipRowFixed(MiniReadBuffer & istr, bool allow_cr_end_of_line);

/// Function-pointer type matching both skipRow() and skipRowFixed(), so
/// countRows() can be driven by either without duplicating the loop.
using SkipRowFn = void (*)(MiniReadBuffer &, bool);

/// RowInputFormatWithNamesAndTypes<CSVFormatReader>::countRows(), copied/adapted
/// — see count_rows.cpp. `skip_row_fn` selects skipRow() or skipRowFixed().
size_t countRows(MiniReadBuffer & buf, size_t max_block_size, bool allow_cr_end_of_line, SkipRowFn skip_row_fn = skipRow);

