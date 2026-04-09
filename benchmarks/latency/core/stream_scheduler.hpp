/// @file core/stream_scheduler.hpp
/// Delta-timer priority queue for multi-stream mock dispatch.
///
/// Used by the exchange WS mock to fire one of N streams (bookTicker,
/// depth, trade, kline) per loop iteration when its individual schedule
/// requires. Each stream carries its own period/jitter policy and an
/// "emit" callback.
///
/// The queue is a `std::priority_queue` keyed by `next_fire_tsc`. Pop
/// only when `now >= top.next_fire_tsc`; pushing the next fire time
/// after each emission turns it into an O(log N) per-tick scheduler.
///
/// All TSCs are raw cycles. Periods are TSC cycles too — convert from
/// chrono once at registration to avoid hot-path division.
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <queue>
#include <random>
#include <vector>

#include "eph/utils/time.hpp"

#include "tsc_protocol.hpp"

namespace bench {

/// One scheduled stream — opaque id + emit callback + interval policy.
struct StreamEntry {
    uint32_t id;                       ///< caller-defined identifier
    uint64_t next_fire_tsc;            ///< absolute TSC for next emit
    uint64_t period_cycles;            ///< 0 = Poisson, >0 = fixed period
    uint64_t poisson_mean_cycles;      ///< only used when period_cycles == 0
    std::function<bool(uint32_t)> emit; ///< returns false on send failure
};

/// Min-heap comparator (smallest next_fire_tsc on top).
struct StreamCompare {
    bool operator()(const StreamEntry* a, const StreamEntry* b) const noexcept {
        return a->next_fire_tsc > b->next_fire_tsc;
    }
};

/// Priority-queue scheduler for multi-stream mock dispatch. Each `tick()`
/// call fires at most one stream whose `next_fire_tsc` has elapsed; the
/// caller drives it in a hot loop, interleaving ticks with other work.
///
/// Not thread-safe — designed for single-threaded mock loops where the
/// enclosing process already pins the thread to a dedicated core.
class StreamScheduler {
public:
    /// Register a fixed-period stream. Returns the entry id.
    /// `period_ns` is converted to TSC cycles once at registration.
    uint32_t register_periodic(uint32_t id, uint64_t period_ns,
                               std::function<bool(uint32_t)> emit) {
        StreamEntry e;
        e.id = id;
        e.period_cycles = bench::tsc::ns_to_cycles(period_ns);
        e.poisson_mean_cycles = 0;
        e.emit = std::move(emit);
        e.next_fire_tsc = eph::utils::TSC::now() + e.period_cycles;
        entries_.push_back(e);
        return id;
    }

    /// Register a Poisson-distributed stream. The next inter-arrival time
    /// is sampled exponentially with the given mean.
    uint32_t register_poisson(uint32_t id, uint64_t mean_ns,
                              std::function<bool(uint32_t)> emit) {
        StreamEntry e;
        e.id = id;
        e.period_cycles = 0;
        e.poisson_mean_cycles = bench::tsc::ns_to_cycles(mean_ns);
        e.emit = std::move(emit);
        e.next_fire_tsc = eph::utils::TSC::now() + sample_exponential(e.poisson_mean_cycles);
        entries_.push_back(e);
        return id;
    }

    /// After all streams are registered, build the heap. Must be called
    /// before `tick()`.
    void build() {
        for (auto& e : entries_) heap_.push(&e);
    }

    /// Reset all streams' next_fire to `now + period` (or sampled). Used
    /// when a client connects so the schedule starts fresh.
    void rebase_now() {
        // Drain heap and rebuild.
        while (!heap_.empty()) heap_.pop();
        uint64_t now = eph::utils::TSC::now();
        for (auto& e : entries_) {
            if (e.period_cycles > 0) {
                e.next_fire_tsc = now + e.period_cycles;
            } else {
                e.next_fire_tsc = now + sample_exponential(e.poisson_mean_cycles);
            }
            heap_.push(&e);
        }
    }

    /// Try to fire the next ready stream. If the head stream's
    /// `next_fire_tsc <= now`, invoke its `emit()` callback, advance
    /// the timer, and return true. Otherwise return false (caller can
    /// do other work then poll again).
    bool tick() {
        if (heap_.empty()) return false;
        StreamEntry* top = heap_.top();
        uint64_t now = eph::utils::TSC::now();
        if (now < top->next_fire_tsc) return false;

        heap_.pop();
        bool ok = top->emit(top->id);

        // Advance timer and reinsert.
        if (top->period_cycles > 0) {
            top->next_fire_tsc += top->period_cycles;
        } else {
            top->next_fire_tsc = now + sample_exponential(top->poisson_mean_cycles);
        }
        heap_.push(top);
        return ok;
    }

    [[nodiscard]] size_t size() const noexcept { return entries_.size(); }

private:
    /// Sample one exponentially-distributed delta in TSC cycles.
    uint64_t sample_exponential(uint64_t mean_cycles) {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        // Inverse-CDF: -mean * ln(1 - u). Guard against u==0 by drawing again.
        for (;;) {
            double r = u(rng_);
            if (r > 0.0 && r < 1.0) {
                double dx = -static_cast<double>(mean_cycles) * std::log(1.0 - r);
                if (dx < 1.0) return 1; // never fire 0 cycles in the future
                return static_cast<uint64_t>(dx);
            }
        }
    }

    std::vector<StreamEntry> entries_;
    std::priority_queue<StreamEntry*, std::vector<StreamEntry*>, StreamCompare> heap_;
    std::mt19937_64 rng_{0xC0FFEE'CAFE'BABEull};
};

} // namespace bench
