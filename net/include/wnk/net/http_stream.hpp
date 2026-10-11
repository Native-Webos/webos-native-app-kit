// SPDX-License-Identifier: GPL-3.0-or-later
//
// A seekable, read-ahead byte reader over one HTTP(S) resource, for demuxers (the FFmpeg media source
// reads network URIs through it).
//
//   open()   sends GET with "Range: bytes=0-" and waits for the response headers. 206 means the server
//            honours ranges (seekable, size from Content-Range); 200 means it does not (size from
//            Content-Length if given; only forward reads work).
//   read()   blocks until bytes at the current position are downloaded. A download thread keeps one
//            transfer open and fills a ring buffer up to `buffer_bytes` ahead of the reader; when the
//            buffer is full it stops reading from the socket (TCP flow control does the rest).
//   seek()   inside the buffer (including `keep_behind_bytes` already read) or a little ahead of the
//            download is free; anything else drops the transfer and starts a new range request.
//
// Transient failures (connect, timeout, reset, truncated body, 502/503/504) reconnect from the first
// byte not yet downloaded, up to `max_retries` times in a row; a reconnect must get 206 for exactly
// that offset and the same total size, otherwise the resource changed and read() fails. Other HTTP
// statuses, TLS failures and invalid URLs fail at once.
//
// Threading: one reader thread calls open/read/seek; interrupt()/reset_interrupt() may be called from
// any thread. The destructor stops and joins the download thread (cancelling its transfer).
#pragma once

#include "wnk/net/http.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace wnk::net {

struct HttpStreamOptions {
    Headers headers;                       // sent with every request (e.g. authorisation)
    int connect_timeout_ms = 5000;
    int stall_timeout_ms = 15000;          // no bytes for this long: reconnect
    size_t buffer_bytes = 8u << 20;        // read-ahead
    size_t keep_behind_bytes = 1u << 20;   // already-read bytes kept for short backward seeks
    size_t seek_wait_bytes = 512u << 10;   // forward seeks this close to the download wait for it
    int max_retries = 3;                   // consecutive failed attempts before read() fails
    int retry_backoff_ms = 300;            // doubled per consecutive failure, capped at 3 s
    int max_redirects = 5;
};

struct HttpStreamStats {
    int requests = 0;            // range requests sent, including reconnects
    int reconnects = 0;          // after a failure
    int64_t bytes_received = 0;
};

class HttpStream {
 public:
    HttpStream(std::shared_ptr<HttpTransport> transport, std::string url, HttpStreamOptions options = {});
    ~HttpStream();
    HttpStream(const HttpStream &) = delete;
    HttpStream &operator=(const HttpStream &) = delete;

    // Blocking. False with error()/error_detail() set if the resource cannot be read.
    bool open();

    int64_t size() const;     // -1 when the server did not say
    bool seekable() const;    // server honours byte ranges
    int64_t position() const;

    // Blocking. Bytes copied (> 0), 0 at the end of the resource, -1 on failure or interrupt
    // (error() is Cancelled for an interrupt).
    int64_t read(void *dst, size_t n);
    // Moves the read position. False (position unchanged) if the stream is not seekable and `pos` is
    // outside the buffer, or if pos is negative. Seeking past the end is allowed; read() returns 0.
    bool seek(int64_t pos);

    void interrupt();
    void reset_interrupt();

    NetError error() const;
    long http_status() const;  // of a failed response, 0 otherwise
    std::string error_detail() const;
    HttpStreamStats stats() const;

    struct Impl;

 private:
    std::unique_ptr<Impl> impl_;
};

// "bytes 0-99/1234" → first=0, last=99, total=1234 (total -1 for "*"). False if malformed.
bool parse_content_range(const std::string &value, int64_t *first, int64_t *last, int64_t *total);

}  // namespace wnk::net
