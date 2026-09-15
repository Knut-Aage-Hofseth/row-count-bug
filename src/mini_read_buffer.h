// mini_read_buffer.h
//
// Minimal reimplementation of just enough of ClickHouse's ReadBuffer/BufferBase
// contract for CSVFormatReader::skipRow() to compile and run unmodified, plus a
// real zlib-backed decompressing buffer that mirrors
// ClickHouse's src/IO/ZlibInflatingReadBuffer.cpp::nextImpl() exactly (one
// inflate(Z_NO_FLUSH) call per refill, output capped at a configurable buf_size,
// same Z_STREAM_END / concatenated-stream handling).
//
// Intentional simplification (input side only): the real ZlibInflatingReadBuffer
// reads its *compressed* input from a separate, independently-buffered
// ReadBuffer. Here we read the whole compressed file into memory once at
// construction and hand it to zlib as a single avail_in region. This does not
// affect the bug under test: the bug is about *output* (decompressed) chunk
// boundaries, which are governed entirely by buf_size/avail_out passed to
// inflate() — not by how the compressed input happens to be chunked on the way
// in. (Project rule: only simplifications proven irrelevant are made — this one
// is justified because decompression output boundaries in zlib depend solely on
// avail_out, not on how avail_in was supplied.)
//
// This file intentionally does NOT touch or reimplement CSVFormatReader::skipRow()
// itself — that function is copied verbatim into skip_row.cpp.

#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

// ---------------------------------------------------------------------------
// Minimal ReadBuffer-alike base class.
//
// Mirrors the exact subset of ClickHouse's IO/BufferBase.h + IO/ReadBuffer.h
// contract that CSVFormatReader::skipRow() actually uses:
//   - Position is char*
//   - position() returns a REFERENCE to the buffer's own internal cursor
//     (this is the crux of the bug: skipRow()'s local `pos` variable is NOT
//     the same storage as position()'s referent, even though they start out
//     holding equal values)
//   - buffer() gives access to .end()
//   - eof() returns true only once no more data can be produced; if the
//     current position has reached the end of the currently loaded chunk,
//     eof() triggers a refill (nextImpl()) first.
// ---------------------------------------------------------------------------
class MiniReadBuffer
{
public:
    using Position = char *;

    virtual ~MiniReadBuffer() = default;

    /// True iff there is no more data at all (after attempting a refill).
    bool eof()
    {
        if (pos < buffer_end_)
            return false;
        return !nextImpl();
    }

    Position & position() { return pos; }

    struct BufferSpan
    {
        Position begin_;
        Position end_;
        Position begin() const { return begin_; }
        Position end() const { return end_; }
    };

    BufferSpan buffer() { return BufferSpan{buffer_begin_, buffer_end_}; }

protected:
    /// Attempt to load more data. On success, must set buffer_begin_/buffer_end_/pos
    /// to describe the newly available chunk (pos == buffer_begin_) and return true.
    /// Returns false only when truly exhausted.
    virtual bool nextImpl() = 0;

    Position pos = nullptr;
    Position buffer_begin_ = nullptr;
    Position buffer_end_ = nullptr;
};

// ---------------------------------------------------------------------------
// Real zlib-backed decompressing MiniReadBuffer.
//
// Mirrors ZlibInflatingReadBuffer::nextImpl() (src/IO/ZlibInflatingReadBuffer.cpp)
// as closely as possible for the *output* side: one inflate(Z_NO_FLUSH) call per
// refill, requesting up to buf_size bytes of decompressed output, same
// Z_STREAM_END / "still-more-input-after-stream-end" (concatenated gzip
// members) handling via inflateReset().
// ---------------------------------------------------------------------------
class ZlibMiniReadBuffer : public MiniReadBuffer
{
public:
    enum class RefillStrategy
    {
        /// Reuse the same fixed output buffer on every refill (overwrite in place).
        /// This is the likely real-world pattern for a performance-sensitive
        /// streaming decompression buffer.
        InPlaceReuse,
        /// Allocate a brand-new output buffer on every refill and free the old
        /// one. Combined with AddressSanitizer, this can catch a stale read of
        /// the old buffer as a tool-confirmed heap-use-after-free.
        FreeAndReallocate,
    };

    ZlibMiniReadBuffer(const std::string & gz_path, size_t buf_size, RefillStrategy strategy)
        : buf_size_(buf_size), strategy_(strategy)
    {
        // Read the whole compressed file into memory once (see file header
        // comment for why this simplification on the *input* side is safe).
        FILE * f = std::fopen(gz_path.c_str(), "rb");
        if (!f)
            throw std::runtime_error("Could not open file: " + gz_path);

        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size < 0)
        {
            std::fclose(f);
            throw std::runtime_error("Could not determine size of file: " + gz_path);
        }

        compressed_data_.resize(static_cast<size_t>(size));
        size_t read = std::fread(compressed_data_.data(), 1, compressed_data_.size(), f);
        std::fclose(f);
        if (read != compressed_data_.size())
            throw std::runtime_error("Short read on file: " + gz_path);

        zstr_.zalloc = nullptr;
        zstr_.zfree = nullptr;
        zstr_.opaque = nullptr;
        zstr_.next_in = compressed_data_.data();
        zstr_.avail_in = static_cast<uInt>(compressed_data_.size());
        zstr_.next_out = nullptr;
        zstr_.avail_out = 0;

        // window_bits = 15 + 16 selects gzip (vs raw zlib/deflate) format,
        // matching ZlibInflatingReadBuffer's CompressionMethod::Gzip branch.
        int rc = inflateInit2(&zstr_, 15 + 16);
        if (rc != Z_OK)
            throw std::runtime_error(std::string("inflateInit2 failed: ") + (zstr_.msg ? zstr_.msg : "?"));
    }

    ~ZlibMiniReadBuffer() override
    {
        inflateEnd(&zstr_);
        if (out_buf_)
            delete[] out_buf_;
    }

protected:
    bool nextImpl() override
    {
        if (eof_flag_)
            return false;

        char * new_out_buf = nullptr;
        if (strategy_ == RefillStrategy::InPlaceReuse)
        {
            if (!out_buf_)
                out_buf_ = new char[buf_size_];
            new_out_buf = out_buf_;
        }
        else // FreeAndReallocate
        {
            new_out_buf = new char[buf_size_];
        }

        zstr_.next_out = reinterpret_cast<unsigned char *>(new_out_buf);
        zstr_.avail_out = static_cast<uInt>(buf_size_);

        int rc = inflate(&zstr_, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END)
        {
            if (strategy_ == RefillStrategy::FreeAndReallocate)
                delete[] new_out_buf;
            throw std::runtime_error(std::string("inflate failed: ") + (zstr_.msg ? zstr_.msg : "?"));
        }

        size_t produced = buf_size_ - zstr_.avail_out;

        if (strategy_ == RefillStrategy::FreeAndReallocate && out_buf_)
            delete[] out_buf_;
        out_buf_ = new_out_buf;

        buffer_begin_ = out_buf_;
        buffer_end_ = out_buf_ + produced;
        pos = buffer_begin_;

        if (rc == Z_STREAM_END)
        {
            if (zstr_.avail_in == 0)
            {
                eof_flag_ = true;
                return produced > 0;
            }
            // Concatenated gzip members (see ZlibInflatingReadBuffer's own
            // comment: "seamlessly decompress multiple concatenated zlib
            // streams"). Reset and, if this call produced nothing, try again.
            rc = inflateReset(&zstr_);
            if (rc != Z_OK)
                throw std::runtime_error(std::string("inflateReset failed: ") + (zstr_.msg ? zstr_.msg : "?"));
            if (produced == 0)
                return nextImpl();
            return true;
        }

        return true;
    }

private:
    size_t buf_size_;
    RefillStrategy strategy_;
    std::vector<unsigned char> compressed_data_;
    z_stream zstr_{};
    bool eof_flag_ = false;
    char * out_buf_ = nullptr;
};
