// SPDX-License-Identifier: GPL-3.0-or-later
//
// Animation helpers driven by the Ui clock (Ui::update(dt) advances it). Nothing here reads the
// system time, so animations are deterministic in tests.
#pragma once

#include <algorithm>
#include <cmath>

namespace wnk::ui {

namespace ease {
inline float linear(float t) { return t; }
inline float out_cubic(float t) {
    const float u = 1 - t;
    return 1 - u * u * u;
}
inline float in_out_cubic(float t) {
    return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2;
}
}  // namespace ease

// Critically damped spring towards a target (used for focus rings and scrolling, where the target
// moves while the animation runs). Unlike an exponential approach it starts gently, keeps its velocity
// when retargeted, and never overshoots. The step is the exact solution, so it is frame-rate
// independent and stable for any dt.
struct Smoothed {
    float value = 0;
    float target = 0;
    float rate = 16;  // angular frequency, 1/s; about 95% of the way in 4.7/rate seconds
    float velocity = 0;

    void snap(float v) {
        value = target = v;
        velocity = 0;
    }
    void step(float dt) {
        const float x = value - target;
        const float e = std::exp(-rate * dt);
        const float k = (velocity + rate * x) * dt;
        value = target + (x + k) * e;
        velocity = (velocity - rate * k) * e;
        if (std::fabs(value - target) < 0.05f && std::fabs(velocity) < 0.5f) snap(target);
    }
    bool settled() const { return value == target && velocity == 0; }
};

// Fixed-duration interpolation with an easing curve.
struct Tween {
    float from = 0, to = 0;
    double start_ms = 0, duration_ms = 0;
    float (*easing)(float) = ease::out_cubic;

    void start(float f, float t, double now_ms, double dur_ms) {
        from = f;
        to = t;
        start_ms = now_ms;
        duration_ms = dur_ms;
    }
    float progress(double now_ms) const {
        if (duration_ms <= 0) return 1;
        return static_cast<float>(std::clamp((now_ms - start_ms) / duration_ms, 0.0, 1.0));
    }
    float value(double now_ms) const { return from + (to - from) * easing(progress(now_ms)); }
    bool done(double now_ms) const { return progress(now_ms) >= 1; }
};

}  // namespace wnk::ui
