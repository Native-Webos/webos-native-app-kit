// SPDX-License-Identifier: GPL-3.0-or-later
//
// Frame timing and process statistics for the perf log and the on-screen metrics overlay. SDL-free,
// main thread only.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace wnk::diagnostics {

// Wall-clock time spent in each part of one main-loop iteration, in milliseconds.
struct FrameSample {
    float interval = 0;  // start of this iteration to the start of the previous one
    float events = 0;    // SDL_PollEvent loop and input handling
    float update = 0;    // dispatcher drain, UI update, app frame hook
    float draw = 0;      // UI draw (on Mali this also holds the wait for a free buffer)
    float finish = 0;    // renderer end_frame, texture budget
    float swap = 0;      // SDL_GL_SwapWindow
};

struct FrameSummary {
    size_t frames = 0;
    float interval_avg = 0, interval_max = 0, interval_p95 = 0;
    size_t late = 0;  // intervals over the late threshold
    float events_max = 0, update_max = 0, draw_max = 0, finish_max = 0, swap_max = 0, swap_avg = 0;
    float fps() const { return interval_avg > 0 ? 1000.0f / interval_avg : 0; }
};

class FrameMetrics {
 public:
    static constexpr size_t kCapacity = 240;  // 4 s at 60 Hz
    explicit FrameMetrics(float late_ms = 20.0f) : late_ms_(late_ms) {}

    void add(const FrameSample &s);
    // Summary of the most recent `window` frames (all retained frames if larger).
    FrameSummary summary(size_t window = kCapacity) const;
    size_t size() const { return count_; }
    uint64_t total() const { return total_; }
    float late_threshold() const { return late_ms_; }
    // i = 0 is the oldest retained sample.
    const FrameSample &at(size_t i) const { return ring_[(head_ + kCapacity - count_ + i) % kCapacity]; }

 private:
    std::array<FrameSample, kCapacity> ring_{};
    size_t head_ = 0;  // next write position
    size_t count_ = 0;
    uint64_t total_ = 0;
    float late_ms_;
};

struct ProcessMemory {
    uint64_t rss_bytes = 0;
    uint64_t virtual_bytes = 0;
    bool ok = false;
};
// From /proc/self/statm.
ProcessMemory process_memory();

}  // namespace wnk::diagnostics
