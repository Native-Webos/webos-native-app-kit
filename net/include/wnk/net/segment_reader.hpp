// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads a list of HTTP byte ranges (an initialisation segment, then media segments) as one forward-only
// byte stream, the way a demuxer sees a DASH representation or an HLS rendition.
//
// A download thread starts at construction and fetches the parts in order into a ring buffer of
// `buffer_bytes`, waiting for the reader when it is full. Each part is one GET, with a Range header
// unless it is a whole resource. A transfer that fails transiently (connect, timeout, reset, truncated
// body, 502/503/504) is retried from the first byte not yet received, up to `max_retries` times in a
// row; a server that ignores the Range header (200 instead of 206) is read from the start and the bytes
// before the range are skipped. Any other status fails the stream at that point: read() returns what
// came before it, then -1.
//
// Each completed part is reported through `on_part_done` with its size and transfer time (time spent
// waiting for room in the buffer excluded), so a caller can estimate throughput (adaptive bitrate).
// stop_after_current() ends the stream at the end of the part being downloaded: the demuxer then
// meets a clean end exactly at a segment boundary, where another rendition can take over.
//
// Live: with `open_ended` the list grows through append() and the stream ends only after finish();
// until then a reader that has caught up waits for the next part.
#pragma once

#include "wnk/net/byte_reader.hpp"
#include "wnk/net/http.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wnk::net {

struct SegmentPart {
    std::string url;
    int64_t offset = 0;
    int64_t length = -1;  // < 0: to the end of the resource
};

struct SegmentReaderOptions {
    Headers headers;                  // sent with every request
    int connect_timeout_ms = 5000;
    int stall_timeout_ms = 15000;     // no bytes for this long: retry
    size_t buffer_bytes = 16u << 20;  // read-ahead
    // Parts fetched beyond the one being read (0: only bytes limit read-ahead). Bounds how much
    // old-quality media a bitrate switch must play out first.
    size_t max_parts_ahead = 0;
    bool open_ended = false;  // live: more parts follow through append() until finish()
    int max_retries = 3;              // consecutive failed attempts on one part
    int retry_backoff_ms = 300;       // doubled per consecutive failure, capped at 3 s
    int max_redirects = 5;
    // Download thread: a part arrived whole (`bytes` in `seconds` of transfer).
    std::function<void(size_t part, int64_t bytes, double seconds)> on_part_done;
};

struct SegmentReaderStats {
    int requests = 0;
    int retries = 0;
    int parts_done = 0;
    int64_t bytes_received = 0;
    double download_seconds = 0;  // transfer time, waiting for room in the buffer excluded
};

class SegmentReader final : public ByteReader {
 public:
    SegmentReader(std::shared_ptr<HttpTransport> transport, std::vector<SegmentPart> parts,
                  SegmentReaderOptions options = {});
    ~SegmentReader() override;
    SegmentReader(const SegmentReader &) = delete;
    SegmentReader &operator=(const SegmentReader &) = delete;

    int64_t read(void *dst, size_t n) override;
    int64_t position() const override;
    void interrupt() override;
    void reset_interrupt() override;
    NetError error() const override;
    long http_status() const override;
    std::string error_detail() const override;

    // Index of the part being downloaded (== parts.size() when all are in).
    size_t download_part() const;
    // Ends the stream after the part being downloaded; returns how many parts will be delivered.
    size_t stop_after_current();
    // Any thread. Open-ended readers: more parts, and the end of the list.
    void append(std::vector<SegmentPart> parts);
    void finish();
    size_t part_count() const;
    SegmentReaderStats stats() const;

    struct Impl;

 private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace wnk::net
