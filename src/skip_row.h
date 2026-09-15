// skip_row.h
//
// Declarations for the verbatim-copied skipRow() (see skip_row.cpp for the
// exact provenance/adaptation notes) and the countRows() driver loop.

#pragma once

#include "mini_read_buffer.h"

/// CSVFormatReader::skipRow(), copied verbatim — see skip_row.cpp.
void skipRow(MiniReadBuffer & istr, bool allow_cr_end_of_line);

/// RowInputFormatWithNamesAndTypes<CSVFormatReader>::countRows(), copied/adapted
/// — see count_rows.cpp.
size_t countRows(MiniReadBuffer & buf, size_t max_block_size, bool allow_cr_end_of_line);
