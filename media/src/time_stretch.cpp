// SPDX-License-Identifier: GPL-3.0-or-later
//
// WSOLA after PlxNative's rust-modules/media/src/speed_audio.rs (Copyright the PlxNative authors,
// GPL-3.0-or-later): same family of algorithm, written for the kit.
#include "wnk/media/time_stretch.hpp"

#include <algorithm>
#include <cmath>

namespace wnk::media {

TimeStretch::TimeStretch(int channels, int sample_rate, double speed)
    : channels_(std::max(1, channels)), speed_(speed) {
    window_ = std::max(64, sample_rate / 50) & ~1;  // 20 ms, even
    hop_ = window_ / 2;
    search_ = std::max(8, sample_rate / 200);  // 5 ms
    hann_.resize(static_cast<size_t>(window_));
    // Periodic Hann: windows half a window apart sum to exactly 1.
    for (int i = 0; i < window_; ++i) hann_[i] = 0.5f - 0.5f * std::cos(2.0 * M_PI * i / window_);
    reset();
}

void TimeStretch::reset() {
    in_.clear();
    mono_.clear();
    in_start_ = 0;
    nominal_ = 0;
    prev_pos_ = -1;
    tail_.assign(static_cast<size_t>(hop_) * channels_, 0.0f);
}

void TimeStretch::process(const float *in, int frames, std::vector<float> *out) {
    if (frames <= 0) return;
    in_.insert(in_.end(), in, in + static_cast<size_t>(frames) * channels_);
    for (int f = 0; f < frames; ++f) {
        float m = 0;
        for (int c = 0; c < channels_; ++c) m += in[static_cast<size_t>(f) * channels_ + c];
        mono_.push_back(m / channels_);
    }
    const int64_t in_end = in_start_ + static_cast<int64_t>(mono_.size());
    for (;;) {
        int64_t pos = std::llround(nominal_);
        if (pos + search_ + window_ > in_end) break;  // not enough input yet
        if (prev_pos_ >= 0) {
            // Where the last window's second half continues in the input: the next window's first half
            // should look like that.
            const int64_t natural = prev_pos_ + hop_;
            if (natural + hop_ <= in_end && natural >= in_start_) {
                const float *ref = mono_.data() + (natural - in_start_);
                double best = -1e30;
                int64_t best_pos = pos;
                const int64_t lo = std::max<int64_t>(in_start_, pos - search_);
                for (int64_t cand = lo; cand <= pos + search_; ++cand) {
                    const float *x = mono_.data() + (cand - in_start_);
                    double dot = 0, energy = 1e-9;
                    for (int i = 0; i < hop_; i += 2) {
                        dot += static_cast<double>(ref[i]) * x[i];
                        energy += static_cast<double>(x[i]) * x[i];
                    }
                    const double score = dot / std::sqrt(energy);
                    if (score > best) {
                        best = score;
                        best_pos = cand;
                    }
                }
                pos = best_pos;
            }
        }
        pos = std::max(pos, in_start_);
        const float *w = in_.data() + static_cast<size_t>(pos - in_start_) * channels_;
        const size_t base = out->size();
        out->resize(base + static_cast<size_t>(hop_) * channels_);
        float *o = out->data() + base;
        for (int i = 0; i < hop_; ++i) {
            for (int c = 0; c < channels_; ++c) {
                const size_t k = static_cast<size_t>(i) * channels_ + c;
                o[k] = tail_[k] + hann_[i] * w[k];
                tail_[k] = hann_[i + hop_] * w[static_cast<size_t>(i + hop_) * channels_ + c];
            }
        }
        prev_pos_ = pos;
        nominal_ += hop_ * speed_;
        // Keep what the next search and window can still reach.
        const int64_t keep_from = std::min<int64_t>(std::llround(nominal_) - search_, prev_pos_ + hop_);
        if (keep_from > in_start_) {
            const size_t drop = static_cast<size_t>(keep_from - in_start_);
            in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(drop * channels_));
            mono_.erase(mono_.begin(), mono_.begin() + static_cast<std::ptrdiff_t>(drop));
            in_start_ = keep_from;
        }
    }
}

void TimeStretch::flush(std::vector<float> *out) {
    if (prev_pos_ < 0) {
        out->insert(out->end(), in_.begin(), in_.end());  // never filled a window: pass it on
    } else {
        out->insert(out->end(), tail_.begin(), tail_.end());
    }
    reset();
}

}  // namespace wnk::media
