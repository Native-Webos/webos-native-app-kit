// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/async/async.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace wnk::async {

// ---- cancellation ------------------------------------------------------------------------------

struct CancellationToken::State {
    std::mutex mutex;
    std::atomic<bool> cancelled{false};
    std::map<uint64_t, std::function<void()>> callbacks;
    uint64_t next_id = 1;
    std::vector<std::weak_ptr<State>> children;
};

namespace {

void cancel_state(const std::shared_ptr<CancellationToken::State> &s) {
    std::map<uint64_t, std::function<void()>> callbacks;
    std::vector<std::weak_ptr<CancellationToken::State>> children;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->cancelled.load()) return;
        s->cancelled.store(true);
        callbacks.swap(s->callbacks);
        children.swap(s->children);
    }
    // Outside the lock: callbacks may register or cancel other tokens.
    for (auto &[id, fn] : callbacks) fn();
    for (auto &w : children) {
        if (auto c = w.lock()) cancel_state(c);
    }
}

}  // namespace

bool CancellationToken::cancelled() const {
    if (state_ && state_->cancelled.load(std::memory_order_acquire)) return true;
    return has_deadline_ && Clock::now() >= deadline_;
}

CancellationToken CancellationToken::with_deadline(Clock::time_point deadline) const {
    CancellationToken t = *this;
    if (!t.has_deadline_ || deadline < t.deadline_) t.deadline_ = deadline;
    t.has_deadline_ = true;
    return t;
}

uint64_t CancellationToken::on_cancel(std::function<void()> fn) const {
    if (!state_) return 0;  // can never be cancelled by a source
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->cancelled.load()) {
            const uint64_t id = state_->next_id++;
            state_->callbacks.emplace(id, std::move(fn));
            return id;
        }
    }
    fn();  // already cancelled: run now, outside the lock
    return 0;
}

void CancellationToken::remove_callback(uint64_t id) const {
    if (!state_ || id == 0) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->callbacks.erase(id);
}

CancellationSource::CancellationSource() : state_(std::make_shared<CancellationToken::State>()) {}

CancellationSource CancellationSource::child() const {
    CancellationSource c;
    bool parent_cancelled = false;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        parent_cancelled = state_->cancelled.load();
        if (!parent_cancelled) {
            // Drop children that no longer exist so long-lived parents do not grow without bound.
            auto &v = state_->children;
            v.erase(std::remove_if(v.begin(), v.end(), [](const auto &w) { return w.expired(); }), v.end());
            v.push_back(c.state_);
        }
    }
    if (parent_cancelled) c.cancel();
    return c;
}

void CancellationSource::cancel() { cancel_state(state_); }

bool CancellationSource::cancelled() const { return state_->cancelled.load(); }

// ---- main-thread dispatcher -------------------------------------------------------------------

void MainDispatcher::set_wake(std::function<void()> wake) {
    std::lock_guard<std::mutex> lock(mutex_);
    wake_ = std::move(wake);
}

bool MainDispatcher::post(Task task) {
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return false;
        queue_.push_back(std::move(task));
        wake = wake_;
    }
    if (wake) wake();
    return true;
}

size_t MainDispatcher::drain(size_t max_tasks, std::chrono::microseconds budget) {
    const auto start = Clock::now();
    size_t limit;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        limit = std::min(max_tasks, queue_.size());  // tasks posted while draining wait for the next call
    }
    size_t ran = 0;
    while (ran < limit) {
        Task task;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) break;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
        ++ran;
        if (budget != std::chrono::microseconds::max() && Clock::now() - start >= budget) break;
    }
    return ran;
}

void MainDispatcher::close() {
    std::deque<Task> dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        dropped.swap(queue_);
    }
    // Destroy dropped tasks outside the lock (their captures may run destructors that post).
}

bool MainDispatcher::closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

size_t MainDispatcher::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

// ---- worker pool --------------------------------------------------------------------------------

struct WorkerPool::Shared {
    std::mutex mutex;
    std::condition_variable work_cv;
    std::condition_variable done_cv;
    std::deque<Item> queue;
    bool stop = false;
    CancellationSource root;
    PoolStats stats;
    size_t exited = 0;
};

WorkerPool::WorkerPool(size_t threads) : shared_(std::make_shared<Shared>()) {
    if (threads == 0) threads = 1;
    shared_->stats.threads = threads;
    threads_.reserve(threads);
    for (size_t i = 0; i < threads; ++i) {
        threads_.emplace_back([s = shared_] { worker_loop(s); });
    }
}

WorkerPool::~WorkerPool() { shutdown(std::chrono::milliseconds(2000)); }

bool WorkerPool::submit(Work work, CancellationToken token, std::function<void()> on_discard) {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (shared_->stop) return false;
    shared_->queue.push_back({std::move(work), std::move(token), std::move(on_discard)});
    shared_->stats.queued = shared_->queue.size();
    shared_->stats.max_queued = std::max(shared_->stats.max_queued, shared_->stats.queued);
    shared_->work_cv.notify_one();
    return true;
}

void WorkerPool::worker_loop(std::shared_ptr<Shared> s) {
    for (;;) {
        Item item;
        {
            std::unique_lock<std::mutex> lock(s->mutex);
            s->work_cv.wait(lock, [&] { return s->stop || !s->queue.empty(); });
            if (s->stop) break;  // queued items are discarded by shutdown()
            item = std::move(s->queue.front());
            s->queue.pop_front();
            s->stats.queued = s->queue.size();
            if (item.token.cancelled()) {
                ++s->stats.cancelled;
                lock.unlock();
                if (item.on_discard) item.on_discard();
                continue;
            }
            ++s->stats.running;
        }

        // The task's token: cancelled by the pool shutting down or by the caller's token, and carrying
        // the caller's deadline.
        CancellationSource task_source = s->root.child();
        const uint64_t link = item.token.on_cancel([task_source]() mutable { task_source.cancel(); });
        CancellationToken task_token = task_source.token();
        if (item.token.has_deadline()) task_token = task_token.with_deadline(item.token.deadline());

        bool failed = false;
        try {
            item.work(task_token);
        } catch (...) {
            failed = true;
        }
        item.token.remove_callback(link);
        item = Item{};  // release captures before reporting completion

        {
            std::lock_guard<std::mutex> lock(s->mutex);
            --s->stats.running;
            if (failed) {
                ++s->stats.failed;
            } else {
                ++s->stats.completed;
            }
        }
        s->done_cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        ++s->exited;
    }
    s->done_cv.notify_all();
}

bool WorkerPool::shutdown(std::chrono::milliseconds deadline) {
    if (shut_down_) return joined_all_;
    shut_down_ = true;
    std::deque<Item> discarded;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->stop = true;
        discarded.swap(shared_->queue);
        shared_->stats.cancelled += discarded.size();
        shared_->stats.queued = 0;
    }
    shared_->root.cancel();
    shared_->work_cv.notify_all();
    for (auto &item : discarded) {
        if (item.on_discard) item.on_discard();
    }

    bool all_exited;
    {
        std::unique_lock<std::mutex> lock(shared_->mutex);
        all_exited = shared_->done_cv.wait_for(lock, deadline, [&] { return shared_->exited == threads_.size(); });
    }
    for (auto &t : threads_) {
        if (all_exited) {
            t.join();
        } else {
            t.detach();  // a task ignored cancellation; its thread keeps the shared state alive
        }
    }
    threads_.clear();
    joined_all_ = all_exited;
    return all_exited;
}

PoolStats WorkerPool::stats() const {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->stats;
}

}  // namespace wnk::async
