// SPDX-License-Identifier: GPL-3.0-or-later
//
// Changes the speed of interleaved float audio without changing its pitch: WSOLA (waveform-similarity
// overlap-add), as PlxNative's speed_audio does for its in-app playback speed. 20 ms Hann windows at
// 50% overlap are taken from the input every hop * speed frames, each one moved by up to 5 ms to where
// its first half best matches the continuation of the window before it (normalised cross-correlation
// over a mono mix, every second sample), and overlap-added into the output every hop frames. Output
// frames are input frames / speed. Pure and single-threaded.
#pragma once

#include <cstdint>
#include <vector>

namespace wnk::media {

class TimeStretch {
 public:
    TimeStretch(int channels, int sample_rate, double speed);
    // Appends what `frames` more input frames make to `out` (interleaved, `channels` per frame).
    void process(const float *in, int frames, std::vector<float> *out);
    // Appends what is still held (the tail of the last window), as at the end of the stream.
    void flush(std::vector<float> *out);
    void reset();  // forget everything (a seek)
    double speed() const { return speed_; }
    int channels() const { return channels_; }

 private:
    int channels_;
    double speed_;
    int window_;   // frames
    int hop_;      // output hop = window / 2
    int search_;   // +- frames
    std::vector<float> hann_;
    std::vector<float> in_;    // interleaved input not yet released
    std::vector<float> mono_;  // its mono mix, for the search
    int64_t in_start_ = 0;     // absolute frame index of in_[0]
    double nominal_ = 0;       // absolute input frame where the next window would start without search
    int64_t prev_pos_ = -1;    // where the last window started
    std::vector<float> tail_;  // its windowed second half, waiting for the next window's first half
};

}  // namespace wnk::media
