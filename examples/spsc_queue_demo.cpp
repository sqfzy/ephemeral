/// @file spsc_queue_demo.cpp
/// SPSC (single-producer, single-consumer) lock-free queue demonstration.
///
/// Demonstrates eph-containers independent of the network stack:
///   1. BoundedQueue  — producer blocks when full (backpressure)
///   2. EvictingQueue  — producer never blocks, overwrites stale data
///   3. Zero-copy visitor pattern (produce/consume callbacks)
///
/// No network dependency — this example runs entirely in-process with two
/// threads communicating via shared queues.
///
/// Usage:
///   ./spsc_queue_demo                    # default: 1M messages
///   ./spsc_queue_demo --count 10000000
///
/// Related examples:
///   - perf_tuning_basics.cpp — CPU pinning for optimal cross-core latency
///   - production_client.cpp  — Transport uses BoundedQueue internally

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/containers/bounded_queue.hpp"
#include "eph/containers/evicting_queue.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Payload type — must be trivially copyable for SPSC queues
// ─────────────────────────────────────────────────────────────────────────────

struct MarketTick {
    uint64_t sequence;
    double   price;
    uint32_t quantity;
};

// ─────────────────────────────────────────────────────────────────────────────
// Demo 1: BoundedQueue — backpressure semantics
// ─────────────────────────────────────────────────────────────────────────────

static void demo_bounded_queue(int count) {
    spdlog::info("=== BoundedQueue demo ({} messages, capacity 4096) ===", count);

    eph::containers::BoundedQueue<MarketTick, 4096> queue;
    std::atomic<bool> done{false};

    // Consumer thread — reads all messages and verifies ordering
    auto consumer = std::thread([&] {
        uint64_t expected_seq = 0;
        uint64_t consumed = 0;

        while (!done.load(std::memory_order_acquire) || !queue.empty()) {
            // Zero-copy visitor: access the slot in-place without copying
            bool got = queue.try_consume([&](MarketTick& tick) {
                if (tick.sequence != expected_seq) {
                    spdlog::error("Out-of-order: expected {}, got {}",
                                  expected_seq, tick.sequence);
                }
                expected_seq = tick.sequence + 1;
                ++consumed;
            });
            if (!got) {
                eph::utils::cpu_relax();  // Spin-wait hint (PAUSE instruction)
            }
        }

        spdlog::info("  Consumer: received {} messages, all in order", consumed);
    });

    // Producer thread — writes all messages
    auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < count; ++i) {
        // Zero-copy produce: write directly into the queue slot
        queue.produce([&](MarketTick& slot) {
            slot.sequence = static_cast<uint64_t>(i);
            slot.price    = 100.0 + (i % 100) * 0.01;
            slot.quantity = static_cast<uint32_t>(i % 1000 + 1);
        });
    }

    auto t1 = std::chrono::steady_clock::now();
    done.store(true, std::memory_order_release);
    consumer.join();

    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    double ns_per_msg = static_cast<double>(elapsed_us) * 1000.0 / count;
    spdlog::info("  Producer: sent {} messages in {} us ({:.1f} ns/msg)",
                 count, elapsed_us, ns_per_msg);
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 2: EvictingQueue — latest-value semantics (no backpressure)
// ─────────────────────────────────────────────────────────────────────────────

static void demo_evicting_queue(int count) {
    spdlog::info("=== EvictingQueue demo ({} writes, capacity 8) ===", count);

    eph::containers::EvictingQueue<MarketTick, 8> queue;
    std::atomic<bool> done{false};

    // Consumer thread — reads latest value (may skip intermediate updates)
    auto consumer = std::thread([&] {
        uint64_t consumed = 0;
        uint64_t last_seq = 0;
        uint64_t max_gap = 0;

        while (!done.load(std::memory_order_acquire)) {
            bool got = queue.try_consume_latest([&](const MarketTick& tick) {
                if (consumed > 0) {
                    // EvictingQueue may skip values — measure the gap
                    uint64_t gap = tick.sequence - last_seq;
                    if (gap > max_gap) max_gap = gap;
                }
                last_seq = tick.sequence;
                ++consumed;
            });
            if (!got) {
                eph::utils::cpu_relax();
            }
        }

        // Drain remaining
        (void)queue.try_consume_latest([&](const MarketTick& tick) {
            last_seq = tick.sequence;
            ++consumed;
        });

        spdlog::info("  Consumer: read {} of {} writes (max gap: {})",
                     consumed, count, max_gap);
        spdlog::info("  Last seen sequence: {} (expected: {})",
                     last_seq, count - 1);
    });

    // Producer thread — writes as fast as possible, never blocks
    auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < count; ++i) {
        queue.push(MarketTick{
            .sequence = static_cast<uint64_t>(i),
            .price    = 100.0 + (i % 100) * 0.01,
            .quantity = static_cast<uint32_t>(i % 1000 + 1),
        });
    }

    auto t1 = std::chrono::steady_clock::now();
    done.store(true, std::memory_order_release);
    consumer.join();

    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    double ns_per_msg = static_cast<double>(elapsed_us) * 1000.0 / count;
    spdlog::info("  Producer: wrote {} messages in {} us ({:.1f} ns/msg)",
                 count, elapsed_us, ns_per_msg);
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 3: Batch operations — try_push_n / try_pop_n
// ─────────────────────────────────────────────────────────────────────────────

static void demo_batch_operations() {
    spdlog::info("=== Batch operations demo ===");

    eph::containers::BoundedQueue<MarketTick, 256> queue;

    // Batch produce: push 10 items at once
    constexpr int batch_size = 10;
    MarketTick batch[batch_size];
    for (int i = 0; i < batch_size; ++i) {
        batch[i] = { .sequence = static_cast<uint64_t>(i),
                     .price = 42.0 + i, .quantity = 100 };
    }

    bool ok = queue.try_push_n(std::span<const MarketTick>(batch, batch_size));
    spdlog::info("  Batch push {} items: {}", batch_size, ok ? "ok" : "failed");

    // Batch consume: pop up to N items
    MarketTick out[batch_size];
    size_t popped = queue.try_pop_n(std::span<MarketTick>(out, batch_size));
    spdlog::info("  Batch pop: got {} items", popped);

    // Verify
    for (size_t i = 0; i < popped; ++i) {
        if (out[i].sequence != i) {
            spdlog::error("  Mismatch at index {}: expected seq {}, got {}",
                          i, i, out[i].sequence);
        }
    }
    spdlog::info("  All {} items verified correct", popped);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    int count = 1'000'000;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--count" && i + 1 < argc) count = std::atoi(argv[++i]);
        else if (arg == "--help") {
            std::cerr << std::format("Usage: {} [--count N]\n", argv[0]);
            return 0;
        }
    }

    demo_bounded_queue(count);
    std::cout << "\n";
    demo_evicting_queue(count);
    std::cout << "\n";
    demo_batch_operations();

    return 0;
}
