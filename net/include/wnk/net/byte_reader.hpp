// SPDX-License-Identifier: GPL-3.0-or-later
//
// A blocking byte source for demuxers: one HTTP resource (HttpStream) or a sequence of byte ranges
// read as one stream (SegmentReader). The media source reads through this interface.
//
// Threading: one reader thread calls read/seek and the accessors; interrupt()/reset_interrupt() may
// be called from any thread.
#pragma once

#include "wnk/net/http.hpp"

#include <cstdint>
#include <string>

namespace wnk::net {

class ByteReader {
 public:
    virtual ~ByteReader() = default;
    // Bytes copied (> 0), 0 at the end, -1 on failure or interrupt (error() is Cancelled then).
    virtual int64_t read(void *dst, size_t n) = 0;
    virtual bool seekable() const { return false; }
    // Moves the read position; false (position unchanged) if that is not possible.
    virtual bool seek(int64_t /*pos*/) { return false; }
    virtual int64_t size() const { return -1; }  // -1 when unknown
    virtual int64_t position() const = 0;
    virtual void interrupt() = 0;
    virtual void reset_interrupt() = 0;
    virtual NetError error() const = 0;
    virtual long http_status() const = 0;  // of a failed response, 0 otherwise
    virtual std::string error_detail() const = 0;
};

}  // namespace wnk::net
