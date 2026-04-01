/// @file perf_tuning_basics.cpp
/// Performance tuning fundamentals — TSC calibration, CPU topology,
/// thread affinity, and huge page memory allocation.
///
/// These primitives are the foundation of low-latency programming with
/// ephemeral. Understanding them is essential before using the network
/// stack in performance-critical deployments.
///
/// Demonstrates:
///   1. TSC (Time Stamp Counter) calibration for nanosecond timing
///   2. CPU topology detection (sockets, cores, hardware threads)
///   3. Thread affinity binding for deterministic scheduling
///   4. Huge page (2MB) memory allocation for reduced TLB misses
///
/// Usage:
///   ./perf_tuning_basics
///
/// Related examples:
///   - production_client.cpp  — uses TSC for latency measurement
///   - spsc_queue_demo.cpp    — benefits from CPU pinning

#include <chrono>
#include <cstdint>
#include <format>
#include <iostream>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu.hpp"
#include "eph/utils/hugepage.hpp"
#include "eph/utils/time.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Demo 1: TSC Calibration
// ─────────────────────────────────────────────────────────────────────────────

static void demo_tsc() {
    spdlog::info("=== TSC (Time Stamp Counter) Calibration ===");

    // TSC must be calibrated once before use. This determines the
    // cycles-to-nanoseconds conversion factor for this CPU.
    spdlog::info("Calibrating TSC (200ms measurement window)...");
    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed — invariant TSC not available?");
        spdlog::error("This typically happens on VMs without TSC passthrough.");
        return;
    }

    auto ns_per_cycle = eph::utils::TSC::get_ns_per_cycle().value();
    spdlog::info("TSC calibrated: {:.4f} ns/cycle", ns_per_cycle);

    // Measure something: cost of TSC::now() itself
    constexpr int N = 1'000'000;
    uint64_t start = eph::utils::TSC::now();
    for (int i = 0; i < N; ++i) {
        [[maybe_unused]] auto t = eph::utils::TSC::now();
    }
    uint64_t end = eph::utils::TSC::now();

    uint64_t total_cycles = end - start;
    double total_ns = eph::utils::TSC::to_ns(total_cycles).value();
    spdlog::info("TSC::now() cost: {:.1f} ns/call ({} calls, {:.0f} ns total)",
                 total_ns / N, N, total_ns);

    // Measure a block of code with TSC::now() bracketing
    uint64_t t0 = eph::utils::TSC::now();
    std::this_thread::sleep_for(std::chrono::microseconds{100});
    uint64_t t1 = eph::utils::TSC::now();
    auto block_ns = eph::utils::TSC::to_ns(t1 - t0).value();
    spdlog::info("TSC measured sleep(100us) as {:.0f} ns ({:.1f} us)",
                 block_ns, block_ns / 1000.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 2: CPU Topology
// ─────────────────────────────────────────────────────────────────────────────

static void demo_cpu_topology() {
    spdlog::info("=== CPU Topology ===");

    auto topo = eph::utils::get_cpu_topology();
    if (!topo) {
        spdlog::error("Failed to detect topology: {}", topo.error());
        return;
    }

    spdlog::info("Detected {} logical CPUs:", topo->size());
    for (const auto& cpu : *topo) {
        // CpuTopologyInfo has std::format support
        spdlog::info("  {}", std::format("{}", cpu));
    }

    // Show CPU base frequency if available
    auto freq = eph::utils::get_cpu_base_frequency();
    if (freq) {
        spdlog::info("Base frequency: {:.0f} MHz", *freq);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 3: Thread Affinity
// ─────────────────────────────────────────────────────────────────────────────

static void demo_thread_affinity() {
    spdlog::info("=== Thread Affinity ===");

    // Pin the current thread to CPU 0
    auto result = eph::utils::set_thread_affinity(0, "demo-thread");
    if (result) {
        spdlog::info("Successfully pinned current thread to CPU 0");
    } else {
        // May fail if running with restricted permissions or in a container
        spdlog::warn("Could not pin to CPU 0: {} (try running as root)", result.error());
    }

    // In production, pin TX and RX threads to separate cores on the same
    // socket to minimize cross-socket traffic. Use TransportConfig::tx_cpu
    // and rx_cpu for automatic pinning in the Transport layer.
    spdlog::info("Tip: use TransportConfig::tx_cpu/rx_cpu for automatic pinning");

    // cpu_relax() — spin-wait hint, reduces power and improves throughput
    // of spin loops by yielding the CPU pipeline.
    spdlog::info("cpu_relax() emits PAUSE (x86) or YIELD (ARM) — use in spin loops");
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 4: Huge Page Allocation
// ─────────────────────────────────────────────────────────────────────────────

static void demo_hugepage() {
    spdlog::info("=== Huge Page Allocation ===");

    // Huge pages (2MB on x86) reduce TLB misses for large allocations.
    // HugePage::make<T>() returns a unique_ptr with custom deleter.
    // Falls back to regular aligned_alloc if huge pages are unavailable.

    // Allocate a 2MB-aligned buffer on a huge page
    struct alignas(64) CacheLine {
        uint8_t data[64];
    };

    // Allocate an array of cache lines via huge page
    constexpr size_t count = 32768;  // 32K * 64 = 2MB
    auto buf = eph::utils::HugePage::make<std::array<CacheLine, count>>();
    if (buf) {
        spdlog::info("Allocated {} cache lines ({} bytes) via HugePage::make",
                     count, count * sizeof(CacheLine));
        spdlog::info("  Address: {} (aligned to 64 bytes: {})",
                     static_cast<void*>(buf.get()),
                     reinterpret_cast<uintptr_t>(buf.get()) % 64 == 0 ? "yes" : "no");

        // Touch the memory to ensure it's mapped
        (*buf)[0].data[0] = 42;
        (*buf)[count - 1].data[0] = 42;
        spdlog::info("  Memory successfully touched (first and last cache line)");
    } else {
        spdlog::warn("HugePage::make returned nullptr — huge pages may not be configured");
        spdlog::info("  To enable: echo 512 > /proc/sys/vm/nr_hugepages");
    }

    // Low-level API for custom allocation sizes
    bool is_hugepage = false;
    size_t allocated_size = 0;
    void* raw = eph::utils::HugePage::allocate(
        2 * 1024 * 1024, 2 * 1024 * 1024, is_hugepage, allocated_size);
    if (raw) {
        spdlog::info("Low-level allocate: {} bytes, hugepage={}, addr={}",
                     allocated_size, is_hugepage, raw);
        eph::utils::HugePage::deallocate(raw, allocated_size, is_hugepage);
        spdlog::info("  Deallocated successfully");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    spdlog::set_level(spdlog::level::info);

    demo_tsc();
    std::cout << "\n";
    demo_cpu_topology();
    std::cout << "\n";
    demo_thread_affinity();
    std::cout << "\n";
    demo_hugepage();

    return 0;
}
