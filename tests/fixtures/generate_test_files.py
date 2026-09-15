#!/usr/bin/env python3
"""Generate synthetic CSV test files for Row-Count-Bug tests.

Builds pairs of small, purely-synthetic CSV files (no real production data —
safe to commit) for a given decompression buffer size:

  boundary_<buf_size>.csv[.gz]  — engineered so one row's CRLF line ending is
                                  split exactly across the buf_size boundary
                                  (\\r is the last byte of chunk 1, \\n is the
                                  first byte of chunk 2) *and* a '\\n' byte
                                  lands at the same buffer-relative offset in
                                  chunk 2 (absolute offset 2*buf_size - 1).

                                  Both coincidences are required to reproduce
                                  the bug with the "in-place reuse" refill
                                  strategy: (1) the stale pointer left over
                                  from chunk 1 must point at a '\\r' right as
                                  the buffer gets refilled, AND (2) whatever
                                  chunk 2 happens to place at that same reused
                                  memory offset must itself be '\\n' for the
                                  stale read to coincidentally "succeed". A
                                  file too short for a full second refill
                                  (verified empirically: an 84-byte file with
                                  buf_size=64) does NOT reproduce the bug,
                                  because the second refill never reaches far
                                  enough to overwrite the stale byte at all.

  control_<buf_size>.csv[.gz]   — same overall structure and row count, but
                                  shifted by one byte so the first CRLF sits
                                  entirely *within* chunk 1 — a negative
                                  control proving the bug is specifically
                                  about the boundary coincidence, not just
                                  "any CRLF-terminated CSV".

Both files' construction is self-validated (asserts the actual bytes at the
computed offsets really are '\\r'/'\\n' as intended) so a math mistake here
fails loudly instead of silently producing a useless fixture.

Usage:
    python3 generate_test_files.py [buf_size] [out_dir]
"""
import gzip
import sys


def _append_row_targeting_offset(out: bytearray, i: int, target_byte_offset: int, target_byte: bytes) -> int:
    """Append filler rows (fixed small value) until close to target_byte_offset,
    then append one row whose content is padded so that `target_byte` (b'\\r'
    or b'\\n') lands at exactly `target_byte_offset`. Returns the next row id.
    """
    while True:
        prefix = f'"{i:06d}",'.encode()
        current_len = len(out)

        if target_byte == b"\r":
            # Row = prefix + '"' + filler + '"' + '\r\n'; '\r' is 2nd-to-last byte
            # of the row: absolute offset = current_len + len(prefix)+F+4 - 2.
            filler_len = target_byte_offset - current_len - len(prefix) - 2
        else:  # b"\n"
            # '\n' is the last byte of the row: absolute offset
            # = current_len + len(prefix)+F+4 - 1 = current_len + len(prefix) + F + 3.
            filler_len = target_byte_offset - current_len - len(prefix) - 3

        if 0 <= filler_len <= 2000:
            filler = "9" * filler_len
            row = prefix + b'"' + filler.encode() + b'"' + b"\r\n"
            out += row
            i += 1
            return i

        # Ordinary filler row, keep going.
        out += prefix + b'"000000"\r\n'
        i += 1
        if i > 10_000:
            raise RuntimeError(f"Could not converge on target_byte_offset={target_byte_offset}")


def build_csv(buf_size: int, shift: int) -> bytes:
    """Build CSV bytes engineering both coincidences needed to reproduce the
    bug under the "in-place reuse" refill strategy (see module docstring).

    shift=0  -> first row's '\\r' at buf_size-1, '\\n' at buf_size (straddles
                the chunk-1/chunk-2 boundary: BUG case), plus a '\\n' at
                2*buf_size-1 (so chunk 2's refill overwrites the stale byte
                with '\\n', completing the coincidence).
    shift=-1 -> first row's '\\r' at buf_size-2 (safely inside chunk 1: no
                boundary straddle at all) — CONTROL case. The second
                coincidence is irrelevant here since there's no stale pointer
                to begin with.
    """
    header = b'"id","value"\r\n'
    out = bytearray(header)
    i = 0

    first_r_offset = buf_size - 1 + shift
    i = _append_row_targeting_offset(out, i, first_r_offset, b"\r")

    if shift == 0:
        # Second coincidence: a '\n' at the buffer-relative-matching offset
        # in chunk 2, needed to complete the bug under in-place reuse.
        second_n_offset = 2 * buf_size - 1
        i = _append_row_targeting_offset(out, i, second_n_offset, b"\n")

    # One trailing row so there's unambiguous content after everything above.
    i += 1
    out += f'"{i:06d}","000001"\r\n'.encode()

    # Self-validate.
    assert out[first_r_offset : first_r_offset + 1] == b"\r", (
        f"Expected '\\r' at offset {first_r_offset}, got {out[first_r_offset:first_r_offset+1]!r}"
    )
    if shift == 0:
        assert out[first_r_offset + 1 : first_r_offset + 2] == b"\n", (
            f"Expected '\\n' at offset {first_r_offset + 1}, got {out[first_r_offset+1:first_r_offset+2]!r}"
        )
        second_n_offset = 2 * buf_size - 1
        assert out[second_n_offset : second_n_offset + 1] == b"\n", (
            f"Expected '\\n' at offset {second_n_offset}, got {out[second_n_offset:second_n_offset+1]!r}"
        )

    return bytes(out)


def count_data_rows(csv_bytes: bytes) -> int:
    """Ground truth: count data rows (excludes header) by counting '\r\n' occurrences minus one."""
    return csv_bytes.count(b"\r\n") - 1


def write_pair(name: str, csv_bytes: bytes, out_dir: str) -> None:
    csv_path = f"{out_dir}/{name}.csv"
    gz_path = f"{out_dir}/{name}.csv.gz"
    with open(csv_path, "wb") as f:
        f.write(csv_bytes)
    with gzip.open(gz_path, "wb") as f:
        f.write(csv_bytes)
    print(f"{name}: {len(csv_bytes)} bytes, {count_data_rows(csv_bytes)} data rows -> {csv_path}, {gz_path}")


def main() -> None:
    buf_size = int(sys.argv[1]) if len(sys.argv) > 1 else 64
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "."

    boundary = build_csv(buf_size, shift=0)
    control = build_csv(buf_size, shift=-1)

    write_pair(f"boundary_{buf_size}", boundary, out_dir)
    write_pair(f"control_{buf_size}", control, out_dir)


if __name__ == "__main__":
    main()

