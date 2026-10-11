// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/app/lifecycle.hpp"
#include "wnk/diagnostics/metrics.hpp"

#include <string>
#include <vector>

using namespace wnk::app;
using wnk::diagnostics::FrameMetrics;
using wnk::diagnostics::FrameSample;

namespace {
std::string path(const Lifecycle &l) {
    std::string s;
    for (const auto &t : l.history()) s += std::string(state_name(t.to)) + " ";
    return s;
}
}  // namespace

TEST("lifecycle: start, background, foreground, first frame, quit") {
    Lifecycle l;
    std::vector<Lifecycle::Transition> seen;
    l.set_listener([&seen](const Lifecycle::Transition &t) { seen.push_back(t); });
    CHECK(l.dispatch(Event::Start, 1));
    CHECK(l.dispatch(Event::Started, 2));
    CHECK(l.renders());
    CHECK(l.dispatch(Event::Background, 3));
    CHECK(!l.renders());
    CHECK(!l.dispatch(Event::Background, 4));  // WILL + DID both arrive: the second is ignored
    CHECK(l.dispatch(Event::Foreground, 5));
    CHECK(l.state() == State::Resuming);
    CHECK(l.renders());
    CHECK(!l.dispatch(Event::Foreground, 6));
    CHECK(l.dispatch(Event::FramePresented, 7));
    CHECK(l.dispatch(Event::Quit, 8));
    CHECK(l.stopping());
    CHECK(l.dispatch(Event::Stopped, 9));
    CHECK_EQ(path(l), std::string("Initialising Running Suspended Resuming Running Stopping Terminated "));
    CHECK_EQ(seen.size(), size_t{7});
    CHECK_EQ(l.ignored(), 2u);
    CHECK(seen[2].event == Event::Background && seen[2].time_ms == 3);
}

TEST("lifecycle: quit is accepted from every live state, nothing leaves Terminated") {
    for (State from : {State::Initialising, State::Running, State::Suspended, State::Resuming}) {
        State to;
        CHECK(Lifecycle::next(from, Event::Quit, &to));
        CHECK(to == State::Stopping);
    }
    State to;
    CHECK(!Lifecycle::next(State::Created, Event::Quit, &to));
    for (Event e : {Event::Start, Event::Started, Event::Background, Event::Foreground, Event::FramePresented,
                    Event::Quit, Event::Stopped}) {
        CHECK(!Lifecycle::next(State::Terminated, e, &to));
        CHECK(!Lifecycle::next(State::Stopping, e, &to) || e == Event::Stopped);
    }
    // Going to the background again before the first frame after resuming.
    CHECK(Lifecycle::next(State::Resuming, Event::Background, &to));
    CHECK(to == State::Suspended);
}

TEST("lifecycle: history is bounded") {
    Lifecycle l;
    l.dispatch(Event::Start, 0);
    l.dispatch(Event::Started, 0);
    for (int i = 0; i < 40; ++i) {
        l.dispatch(Event::Background, i);
        l.dispatch(Event::Foreground, i);
        l.dispatch(Event::FramePresented, i);
    }
    CHECK_EQ(l.history().size(), size_t{32});
    CHECK(l.history().back().to == State::Running);
}

TEST("frame metrics: averages, maxima, p95 and late frames over a window") {
    FrameMetrics m(20.0f);
    for (int i = 0; i < 100; ++i) {
        FrameSample s;
        s.interval = (i % 10 == 9) ? 33.3f : 16.7f;
        s.draw = static_cast<float>(i);
        s.swap = 1.0f;
        m.add(s);
    }
    const auto all = m.summary();
    CHECK_EQ(all.frames, size_t{100});
    CHECK_EQ(all.late, size_t{10});
    CHECK(all.interval_max > 33.0f);
    CHECK(all.interval_p95 > 33.0f);  // 10% of frames are late, so p95 is a late frame
    CHECK(all.interval_avg > 18.3f && all.interval_avg < 18.4f);
    CHECK_EQ(all.draw_max, 99.0f);
    CHECK_EQ(all.swap_avg, 1.0f);
    const auto last5 = m.summary(5);
    CHECK_EQ(last5.frames, size_t{5});
    CHECK_EQ(last5.late, size_t{1});
    CHECK_EQ(last5.draw_max, 99.0f);
}

TEST("frame metrics: ring keeps the newest samples in order") {
    FrameMetrics m;
    for (size_t i = 0; i < FrameMetrics::kCapacity + 10; ++i) {
        FrameSample s;
        s.interval = static_cast<float>(i);
        m.add(s);
    }
    CHECK_EQ(m.size(), FrameMetrics::kCapacity);
    CHECK_EQ(m.total(), static_cast<uint64_t>(FrameMetrics::kCapacity + 10));
    CHECK_EQ(m.at(0).interval, 10.0f);
    CHECK_EQ(m.at(m.size() - 1).interval, static_cast<float>(FrameMetrics::kCapacity + 9));
    CHECK(wnk::diagnostics::process_memory().ok);
}
