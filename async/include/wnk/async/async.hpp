// SPDX-License-Identifier: GPL-3.0-or-later
//
// Background work for TV apps: cancellation, a worker pool, a main-thread dispatcher and lifetime
// guards for callbacks.
//
// Threading model (ADR-008):
//   * the main thread owns SDL, GL and the UI and drains a MainDispatcher once per frame;
//   * blocking work (DB8, HTTP, file I/O, decoding) runs on a WorkerPool;
//   * workers hand results back with MainDispatcher::post(); they never wait for the main thread,
//     so shutdown cannot deadlock on a UI that has stopped drawing;
//   * a callback captures a Lifetime::Weak of its owner and is skipped if the owner was destroyed.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace wnk::async {

using Clock = std::chrono::steady_clock;

// ---- cancellation ------------------------------------------------------------------------------

class CancellationSource;

// Cheap to copy; observes one CancellationSource (or none: never cancelled). A token can also carry a
// deadline, after which cancelled() is true even if nobody called cancel().
class CancellationToken {
 public:
    CancellationToken() = default;
    bool cancelled() const;
    bool can_be_cancelled() const { return state_ != nullptr || has_deadline_; }
    // Same source, with a deadline (the earlier of this and any existing deadline).
    CancellationToken with_deadline(Clock::time_point deadline) const;
    CancellationToken with_timeout(std::chrono::milliseconds timeout) const {
        return with_deadline(Clock::now() + timeout);
    }
    Clock::time_point deadline() const { return deadline_; }
    bool has_deadline() const { return has_deadline_; }

    // Runs `fn` once when the source is cancelled (immediately if it already is). The returned id
    // unregisters it; unregistering after it ran is harmless. Deadlines do not trigger callbacks.
    uint64_t on_cancel(std::function<void()> fn) const;
    void remove_callback(uint64_t id) const;

    struct State;

 private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<State> s) : state_(std::move(s)) {}
    std::shared_ptr<State> state_;
    Clock::time_point deadline_{};
    bool has_deadline_ = false;
};

class CancellationSource {
 public:
    CancellationSource();
    // A child is cancelled when this source is, and can also be cancelled on its own.
    CancellationSource child() const;
    CancellationToken token() const { return CancellationToken(state_); }
    void cancel();
    bool cancelled() const;

 private:
    std::shared_ptr<CancellationToken::State> state_;
};

// ---- main-thread dispatcher -------------------------------------------------------------------

// Multi-producer queue drained by one consumer (the main thread).
class MainDispatcher {
 public:
    using Task = std::function<void()>;

    // Called (from the posting thread) after each successful post, e.g. to push an SDL user event so
    // an idle loop wakes up. Must be cheap and must not call back into the dispatcher.
    void set_wake(std::function<void()> wake);

    // Returns false (and drops the task) once close() was called.
    bool post(Task task);

    // Runs queued tasks in FIFO order until the queue is empty, `max_tasks` ran, or `budget` elapsed
    // (checked between tasks). Tasks posted while draining run on the next call. Returns the count.
    size_t drain(size_t max_tasks = SIZE_MAX, std::chrono::microseconds budget = std::chrono::microseconds::max());

    // Stops accepting tasks and discards anything not yet run. Idempotent.
    void close();
    bool closed() const;
    size_t pending() const;

 private:
    mutable std::mutex mutex_;
    std::deque<Task> queue_;
    bool closed_ = false;
    std::function<void()> wake_;
};

// ---- lifetime guard -------------------------------------------------------------------------------

// Owners (screens, widgets, services) hold a Lifetime; callbacks capture weak() and call alive() on the
// main thread before touching the owner. Destroying the Lifetime makes every Weak report dead.
class Lifetime {
 public:
    class Weak {
     public:
        Weak() = default;
        bool alive() const { return !flag_.expired(); }

     private:
        friend class Lifetime;
        explicit Weak(std::weak_ptr<int> f) : flag_(std::move(f)) {}
        std::weak_ptr<int> flag_;
    };
    Lifetime() : flag_(std::make_shared<int>(0)) {}
    Lifetime(const Lifetime &) = delete;
    Lifetime &operator=(const Lifetime &) = delete;
    Weak weak() const { return Weak(flag_); }

 private:
    std::shared_ptr<int> flag_;
};

// ---- worker pool --------------------------------------------------------------------------------

struct PoolStats {
    size_t threads = 0;
    size_t queued = 0;        // submitted, not started
    size_t running = 0;
    uint64_t completed = 0;   // finished normally
    uint64_t cancelled = 0;   // discarded before starting because cancelled or shut down
    uint64_t failed = 0;      // threw an exception (caught and counted)
    size_t max_queued = 0;    // high-water mark
};

class WorkerPool {
 public:
    using Work = std::function<void(const CancellationToken &)>;

    explicit WorkerPool(size_t threads);
    ~WorkerPool();  // shutdown(2 s)
    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

    // Queues `work`. It receives a token that is cancelled when `token` is, or when the pool shuts
    // down. If cancelled before it starts it never runs; `on_discard`, if given, runs instead (on the
    // thread that discovers it). Returns false after shutdown began.
    bool submit(Work work, CancellationToken token = {}, std::function<void()> on_discard = {});

    // Cancels every task, discards queued ones, and waits up to `deadline` for running ones to return.
    // Returns true if every thread was joined. Returns false if a task ignored cancellation past the
    // deadline: those threads are detached (their state is shared, so this stays memory-safe) and the
    // caller should log it. Idempotent.
    bool shutdown(std::chrono::milliseconds deadline);

    PoolStats stats() const;

 private:
    struct Item {
        Work work;
        CancellationToken token;
        std::function<void()> on_discard;
    };
    struct Shared;
    static void worker_loop(std::shared_ptr<Shared> shared);  // static: detached threads may outlive the pool

    std::shared_ptr<Shared> shared_;
    std::vector<std::thread> threads_;
    bool shut_down_ = false;
    bool joined_all_ = true;
};

// ---- helpers ---------------------------------------------------------------------------------------

// Run `work` on the pool, then deliver its result on the main thread through `dispatcher`, unless the
// owner died or the token was cancelled meanwhile. `on_result` runs only on the main thread.
template <typename T>
bool run_then_post(WorkerPool &pool, MainDispatcher &dispatcher, Lifetime::Weak owner, CancellationToken token,
                   std::function<T(const CancellationToken &)> work, std::function<void(T)> on_result) {
    return pool.submit(
        [&dispatcher, owner, token, work = std::move(work), on_result = std::move(on_result)](
            const CancellationToken &t) mutable {
            T result = work(t);
            if (t.cancelled() || !owner.alive()) return;
            auto shared = std::make_shared<T>(std::move(result));
            dispatcher.post([owner, token, shared, on_result = std::move(on_result)]() {
                if (!owner.alive() || token.cancelled()) return;
                on_result(std::move(*shared));
            });
        },
        token);
}

}  // namespace wnk::async
