#pragma once
// M5.1: bounded worker-queue primitive.
// `parallel_map` runs `f` over `items` across up to `threads` worker threads,
// returning results in input order. It bounds in-flight work to O(threads) by
// driving a shared atomic cursor (no per-worker unbounded buffering), supports
// cooperative cancellation (an optional atomic flag), propagates the first task
// exception after joining every worker, and falls back to a plain inline loop
// when `threads <= 1` or there is a single item — so the observable result is
// byte-identical to a serial pass.
//
// The result element type R must be default-constructible only if a task is
// cancelled before it runs; completed tasks require only moveable R. This is
// sufficient for std::vector<Match> and scalar workloads.
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace pergrep {
namespace detail {

// items : std::vector<T> ; f : const T& -> R. Returns results in input order.
template <class T, class F>
auto parallel_map(const std::vector<T>& items, unsigned threads, F&& f,
                  std::atomic<bool>* cancel = nullptr)
    -> std::vector<std::decay_t<decltype(f(items.front()))>> {
    using R = std::decay_t<decltype(f(items.front()))>;
    const std::size_t n = items.size();
    std::vector<R> out; out.reserve(n);

    if (n == 0) return out;
    if (threads <= 1u || n == 1) {
        // Single-thread fallback: identical to a plain serial loop.
        for (std::size_t i = 0; i < n; ++i) {
            if (cancel && cancel->load(std::memory_order_relaxed)) break;
            out.push_back(f(items[i]));
        }
        return out;
    }

    // Per-slot storage preserves input order and decouples task completion from
    // scheduling order. A nullopt slot marks a task never started (cancellation
    // observed first, or an earlier failure).
    std::vector<std::optional<R>> slots(n);
    std::atomic<std::size_t> cursor{0};
    std::mutex exc_mtx;
    std::exception_ptr first_exc = nullptr;

    const auto worker = [&]() {
        for (;;) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return;
            const std::size_t i = cursor.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) return;
            try {
                R r = f(items[i]);
                slots[i].emplace(std::move(r));
            } catch (...) {
                std::lock_guard<std::mutex> lk(exc_mtx);
                if (!first_exc) first_exc = std::current_exception();
                return;
            }
        }
    };

    const unsigned w = (n < static_cast<std::size_t>(threads)) ? static_cast<unsigned>(n) : threads;
    std::vector<std::thread> pool;
    pool.reserve(w);
    for (unsigned k = 0; k < w; ++k) pool.emplace_back(worker);
    for (auto& t : pool) t.join();  // join on every path (exception/cancel/normal)

    if (first_exc) std::rethrow_exception(first_exc);

    for (std::size_t i = 0; i < n; ++i) {
        if (slots[i].has_value()) out.push_back(std::move(*slots[i]));
    }
    return out;
}

} // namespace detail
} // namespace pergrep