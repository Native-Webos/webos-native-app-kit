// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/time_stretch.hpp"

#include <cmath>
#include <vector>

using namespace wnk::media;

namespace {
constexpr int kRate = 48000;

std::vector<float> sine(double hz, double seconds, int channels) {
    const int n = static_cast<int>(seconds * kRate);
    std::vector<float> v(static_cast<size_t>(n) * channels);
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < channels; ++c) v[static_cast<size_t>(i) * channels + c] = 0.5f * std::sin(2 * M_PI * hz * i / kRate);
    }
    return v;
}

struct Measured {
    double seconds = 0, hz = 0, max_step = 0, rms = 0;
};

// Over the middle of channel 0 (away from the start-up and the flushed tail).
Measured measure(const std::vector<float> &v, int channels) {
    Measured m;
    const size_t frames = v.size() / channels;
    m.seconds = static_cast<double>(frames) / kRate;
    const size_t a = frames / 5, b = frames - frames / 5;
    int crossings = 0;
    double sq = 0;
    for (size_t i = a + 1; i < b; ++i) {
        const float x0 = v[(i - 1) * channels], x1 = v[i * channels];
        if (x0 < 0 && x1 >= 0) ++crossings;
        m.max_step = std::max(m.max_step, static_cast<double>(std::fabs(x1 - x0)));
        sq += static_cast<double>(x1) * x1;
    }
    m.hz = crossings / (static_cast<double>(b - a) / kRate);
    m.rms = std::sqrt(sq / (b - a));
    return m;
}

Measured run(double speed, int channels, int chunk) {
    const auto in = sine(440, 3.0, channels);
    TimeStretch t(channels, kRate, speed);
    std::vector<float> out;
    const int frames = static_cast<int>(in.size() / channels);
    for (int f = 0; f < frames; f += chunk) {
        t.process(in.data() + static_cast<size_t>(f) * channels, std::min(chunk, frames - f), &out);
    }
    t.flush(&out);
    return measure(out, channels);
}
}  // namespace

TEST("time stretch: the length scales by 1/speed and the pitch stays, at every speed offered") {
    for (const double speed : {0.5, 0.75, 1.25, 1.5, 1.75, 2.0}) {
        const Measured m = run(speed, 2, 1024);
        CHECK(std::fabs(m.seconds - 3.0 / speed) < 0.05);
        CHECK(std::fabs(m.hz - 440) < 4);  // pitch kept
        // A sine of amplitude 0.5 at 440 Hz moves at most 0.029 per sample: no click at the joins.
        CHECK(m.max_step < 0.06);
        CHECK(std::fabs(m.rms - 0.3536) < 0.03);  // loudness kept (0.5 / sqrt 2)
    }
}

TEST("time stretch: chunk size does not matter; mono and 6 channels work; reset starts over") {
    const Measured a = run(1.5, 2, 1024), b = run(1.5, 2, 37);
    CHECK(std::fabs(a.seconds - b.seconds) < 0.002);
    CHECK(std::fabs(a.hz - b.hz) < 1);
    CHECK(std::fabs(run(1.25, 1, 500).hz - 440) < 4);
    CHECK(std::fabs(run(0.75, 6, 500).hz - 440) < 4);
    TimeStretch t(2, kRate, 2.0);
    const auto in = sine(440, 0.5, 2);
    std::vector<float> out;
    t.process(in.data(), static_cast<int>(in.size() / 2), &out);
    t.reset();
    std::vector<float> again;
    t.process(in.data(), static_cast<int>(in.size() / 2), &again);
    CHECK(out == again);  // same input after a reset, same output
}
