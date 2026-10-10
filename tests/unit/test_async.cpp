// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/async/async.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace wnk::async;
using namespace std::chrono_literals;

namespace {

// Spin-wait with a hard limit so a broken implementation fails instead of hanging the test run.
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds limit = 5000ms) {
    const auto end = Clock::now() + limit;
    while (!pred()) {
        if (Clock::now() > end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

}  // namespace

TEST("default token is never cancelled") {
    CancellationToken t;
    CHECK(!t.cancelled());
    CHECK(!t.can_be_cancelled());
    CHECK_EQ(t.on_cancel([] {}), 0u);
}

TEST("cancel runs callbacks once, late registration runs immediately") {
    CancellationSource src;
    auto tok = src.token();
    int calls = 0;
    tok.on_cancel([&] { ++calls; });
    const uint64_t removed = tok.on_cancel([&] { calls += 100; });
    tok.remove_callback(removed);
    CHECK(!tok.cancelled());
    src.cancel();
    src.cancel();
    CHECK(tok.cancelled());
    CHECK_EQ(calls, 1);
    tok.on_cancel([&] { calls += 10; });
    CHECK_EQ(calls, 11);
}

TEST("children follow their parent but not the other way round") {
    CancellationSource parent;
    CancellationSource a = parent.child();
    CancellationSource b = parent.child();
    a.cancel();
    CHECK(a.cancelled());
    CHECK(!parent.cancelled());
    CHECK(!b.cancelled());
    parent.cancel();
    CHECK(b.cancelled());
    CancellationSource late = parent.child();
    CHECK(late.cancelled());
}

TEST("deadlines cancel tokens without a source") {
    CancellationToken t = CancellationToken().with_timeout(20ms);
    CHECK(t.can_be_cancelled());
    CHECK(!t.cancelled());
    std::this_thread::sleep_for(30ms);
    CHECK(t.cancelled());
    CancellationToken earlier = t.with_deadline(Clock::now() + 1h);
    CHECK(earlier.deadline() == t.deadline());  // keeps the earlier deadline
}

TEST("dispatcher runs tasks in order on the draining thread") {
    MainDispatcher d;
    std::vector<int> order;
    const auto main_id = std::this_thread::get_id();
    bool all_on_main = true;
    std::thread producer([&] {
        for (int i = 0; i < 100; ++i) {
            d.post([&, i] {
                order.push_back(i);
                all_on_main = all_on_main && std::this_thread::get_id() == main_id;
            });
        }
    });
    producer.join();
    CHECK_EQ(d.pending(), 100u);
    CHECK_EQ(d.drain(), 100u);
    REQUIRE(order.size() == 100u);
    for (int i = 0; i < 100; ++i) CHECK_EQ(order[static_cast<size_t>(i)], i);
    CHECK(all_on_main);
}

TEST("dispatcher respects max tasks and defers tasks posted while draining") {
    MainDispatcher d;
    int ran = 0;
    for (int i = 0; i < 5; ++i) d.post([&] { ++ran; });
    CHECK_EQ(d.drain(2), 2u);
    CHECK_EQ(ran, 2);
    d.post([&] { d.post([&] { ++ran; }); });  // re-posts itself once
    CHECK_EQ(d.drain(), 4u);                   // 3 remaining + the re-poster, not the re-posted task
    CHECK_EQ(d.pending(), 1u);
    CHECK_EQ(d.drain(), 1u);
    CHECK_EQ(ran, 6);
}

TEST("dispatcher time budget stops early") {
    MainDispatcher d;
    for (int i = 0; i < 10; ++i) d.post([] { std::this_thread::sleep_for(5ms); });
    const size_t ran = d.drain(SIZE_MAX, std::chrono::microseconds(12000));
    CHECK(ran >= 1 && ran <= 4);
    CHECK_EQ(d.pending(), 10u - ran);
}

TEST("closed dispatcher rejects and discards") {
    MainDispatcher d;
    int wakes = 0;
    d.set_wake([&] { ++wakes; });
    bool ran = false;
    CHECK(d.post([&] { ran = true; }));
    CHECK_EQ(wakes, 1);
    d.close();
    CHECK(d.closed());
    CHECK(!d.post([&] { ran = true; }));
    CHECK_EQ(d.drain(), 0u);
    CHECK(!ran);
    CHECK_EQ(wakes, 1);
}

TEST("pool runs every task and counts them") {
    WorkerPool pool(4);
    std::atomic<int> sum{0};
    for (int i = 1; i <= 100; ++i) pool.submit([&, i](const CancellationToken &) { sum += i; });
    CHECK(wait_until([&] { return pool.stats().completed == 100; }));
    CHECK_EQ(sum.load(), 5050);
    CHECK_EQ(pool.stats().threads, 4u);
    CHECK(pool.shutdown(1000ms));
}

TEST("tasks cancelled before they start are discarded") {
    WorkerPool pool(1);
    std::atomic<bool> release{false};
    pool.submit([&](const CancellationToken &) { wait_until([&] { return release.load(); }); });
    CancellationSource src;
    std::atomic<bool> ran{false}, discarded{false};
    pool.submit([&](const CancellationToken &) { ran = true; }, src.token(), [&] { discarded = true; });
    src.cancel();
    release = true;
    CHECK(wait_until([&] { return discarded.load(); }));
    CHECK(!ran.load());
    CHECK(wait_until([&] { return pool.stats().cancelled == 1; }));
    CHECK(pool.shutdown(1000ms));
}

TEST("caller cancellation and deadlines reach a running task") {
    WorkerPool pool(2);
    CancellationSource src;
    std::atomic<bool> saw_cancel{false}, saw_deadline{false}, started{false};
    pool.submit(
        [&](const CancellationToken &t) {
            started = true;
            saw_cancel = wait_until([&] { return t.cancelled(); }, 3000ms);
        },
        src.token());
    pool.submit([&](const CancellationToken &t) { saw_deadline = wait_until([&] { return t.cancelled(); }, 3000ms); },
                CancellationToken().with_timeout(30ms));
    CHECK(wait_until([&] { return started.load(); }));
    src.cancel();
    CHECK(wait_until([&] { return pool.stats().completed == 2; }));
    CHECK(saw_cancel.load());
    CHECK(saw_deadline.load());
    CHECK(pool.shutdown(1000ms));
}

TEST("shutdown cancels running tasks and discards queued ones") {
    WorkerPool pool(1);
    std::atomic<bool> running{false}, stopped_by_cancel{false};
    std::atomic<int> discards{0};
    pool.submit([&](const CancellationToken &t) {
        running = true;
        stopped_by_cancel = wait_until([&] { return t.cancelled(); }, 3000ms);
    });
    for (int i = 0; i < 5; ++i) pool.submit([](const CancellationToken &) {}, {}, [&] { ++discards; });
    CHECK(wait_until([&] { return running.load(); }));
    CHECK(pool.shutdown(2000ms));
    CHECK(stopped_by_cancel.load());
    CHECK_EQ(discards.load(), 5);
    CHECK(!pool.submit([](const CancellationToken &) {}));
    CHECK(pool.shutdown(10ms));  // idempotent, same answer
}

TEST("shutdown reports a task that ignores cancellation instead of hanging") {
    auto stubborn_done = std::make_shared<std::atomic<bool>>(false);
    WorkerPool pool(1);
    pool.submit([stubborn_done](const CancellationToken &) {
        std::this_thread::sleep_for(300ms);  // ignores its token
        *stubborn_done = true;
    });
    std::this_thread::sleep_for(20ms);
    const auto t0 = Clock::now();
    CHECK(!pool.shutdown(50ms));
    CHECK(Clock::now() - t0 < 250ms);
    CHECK(!pool.shutdown(10ms));  // still reports the failure
    CHECK(wait_until([&] { return stubborn_done->load(); }));  // the detached thread finished safely
}

TEST("exceptions in tasks are contained and counted") {
    WorkerPool pool(1);
    pool.submit([](const CancellationToken &) { throw std::runtime_error("boom"); });
    std::atomic<bool> after{false};
    pool.submit([&](const CancellationToken &) { after = true; });
    CHECK(wait_until([&] { return after.load(); }));
    CHECK(wait_until([&] { return pool.stats().failed == 1; }));
    CHECK(pool.shutdown(1000ms));
}

TEST("run_then_post delivers on the main thread and drops results for dead owners") {
    WorkerPool pool(2);
    MainDispatcher main;
    std::string got;
    {
        Lifetime owner;
        run_then_post<std::string>(pool, main, owner.weak(), {},
                                   [](const CancellationToken &) { return std::string("payload"); },
                                   [&](std::string s) { got = std::move(s); });
        CHECK(wait_until([&] { return main.pending() == 1; }));
        main.drain();
        CHECK_EQ(got, std::string("payload"));
    }
    got.clear();
    {
        auto owner = std::make_unique<Lifetime>();
        run_then_post<std::string>(pool, main, owner->weak(), {},
                                   [](const CancellationToken &) { return std::string("late"); },
                                   [&](std::string s) { got = std::move(s); });
        CHECK(wait_until([&] { return main.pending() == 1 || pool.stats().completed == 2; }));
        owner.reset();  // the screen went away before the result was delivered
        main.drain();
        CHECK(got.empty());
    }
    CHECK(pool.shutdown(1000ms));
}

TEST("workers posting to a closed dispatcher during shutdown do not block") {
    WorkerPool pool(4);
    MainDispatcher main;
    std::atomic<int> posted{0};
    for (int i = 0; i < 200; ++i) {
        pool.submit([&](const CancellationToken &) {
            if (main.post([] {})) ++posted;
        });
    }
    main.close();  // the UI is gone; nothing drains anymore
    CHECK(pool.shutdown(2000ms));
    CHECK_EQ(main.pending(), 0u);
}
