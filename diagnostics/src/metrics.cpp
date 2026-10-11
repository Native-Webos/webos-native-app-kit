// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/diagnostics/metrics.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

#include <unistd.h>

namespace wnk::diagnostics {

void FrameMetrics::add(const FrameSample &s) {
    ring_[head_] = s;
    head_ = (head_ + 1) % kCapacity;
    count_ = std::min(count_ + 1, kCapacity);
    ++total_;
}

FrameSummary FrameMetrics::summary(size_t window) const {
    FrameSummary out;
    const size_t n = std::min(window, count_);
    if (n == 0) return out;
    std::vector<float> intervals;
    intervals.reserve(n);
    double interval_sum = 0, swap_sum = 0;
    for (size_t i = count_ - n; i < count_; ++i) {
        const FrameSample &s = at(i);
        intervals.push_back(s.interval);
        interval_sum += s.interval;
        swap_sum += s.swap;
        out.interval_max = std::max(out.interval_max, s.interval);
        out.events_max = std::max(out.events_max, s.events);
        out.update_max = std::max(out.update_max, s.update);
        out.draw_max = std::max(out.draw_max, s.draw);
        out.finish_max = std::max(out.finish_max, s.finish);
        out.swap_max = std::max(out.swap_max, s.swap);
        if (s.interval > late_ms_) ++out.late;
    }
    out.frames = n;
    out.interval_avg = static_cast<float>(interval_sum / n);
    out.swap_avg = static_cast<float>(swap_sum / n);
    const size_t k = std::min(n - 1, static_cast<size_t>(n * 0.95));
    std::nth_element(intervals.begin(), intervals.begin() + static_cast<long>(k), intervals.end());
    out.interval_p95 = intervals[k];
    return out;
}

ProcessMemory process_memory() {
    ProcessMemory m;
    FILE *f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) return m;
    unsigned long long size = 0, resident = 0;
    if (std::fscanf(f, "%llu %llu", &size, &resident) == 2) {
        const long page = sysconf(_SC_PAGESIZE);
        m.virtual_bytes = size * static_cast<uint64_t>(page);
        m.rss_bytes = resident * static_cast<uint64_t>(page);
        m.ok = true;
    }
    std::fclose(f);
    return m;
}

}  // namespace wnk::diagnostics
