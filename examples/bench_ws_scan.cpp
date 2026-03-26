/// @file bench_ws_scan.cpp
/// Micro-benchmark: WS frame processing strategies for multi-symbol streams.
///
/// Compares three approaches for processing a buffer of bookTicker WS frames
/// containing messages for multiple symbols:
///
///   Baseline:     Deliver every frame (current deferred-stats approach)
///   Optimization A: Forward scan + reverse dedup (per-symbol last-only)
///   Optimization B: Two-phase index + selective deliver
///
/// Uses synthetic data matching real Binance bookTicker format.
/// No network, no TLS — pure CPU hot-path measurement.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/containers/evicting_queue.hpp"
#include "eph/net/websocket.hpp"
#include "eph/utils/time.hpp"

// ---------------------------------------------------------------------------
// Synthetic data generation
// ---------------------------------------------------------------------------

static const std::array<std::string, 3> kSymbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};

/// Generate a bookTicker JSON payload for a given symbol.
static std::string make_bookticker(const std::string& symbol, uint64_t event_time) {
    return std::format(
        R"({{"e":"bookTicker","u":{},"E":{},"T":{},"s":"{}","b":"67123.45","B":"1.234","a":"67124.56","A":"0.567"}})",
        event_time * 100, event_time, event_time, symbol);
}

/// Encode a WS text frame (server-to-client, unmasked, FIN=1).
static std::vector<uint8_t> encode_ws_frame(std::string_view payload) {
    std::vector<uint8_t> frame;
    // FIN=1, opcode=text(1)
    frame.push_back(0x81);
    // Payload length (no mask for server frames)
    if (payload.size() < 126) {
        frame.push_back(static_cast<uint8_t>(payload.size()));
    } else if (payload.size() < 65536) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>(payload.size() >> 8));
        frame.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

/// Build a buffer of N WS frames cycling through kSymbols.
static std::vector<uint8_t> build_ws_buffer(size_t n_frames) {
    std::vector<uint8_t> buf;
    for (size_t i = 0; i < n_frames; ++i) {
        auto json = make_bookticker(kSymbols[i % kSymbols.size()], 1700000000000ULL + i);
        auto frame = encode_ws_frame(json);
        buf.insert(buf.end(), frame.begin(), frame.end());
    }
    return buf;
}

// ---------------------------------------------------------------------------
// Processing strategies
// ---------------------------------------------------------------------------

/// Simulates EvictingQueue push overhead (2 release fences + atomic store).
/// Using volatile + memory fence to prevent optimizer from removing the work.
static volatile uint64_t g_sink = 0;

static void simulate_enqueue(const uint8_t* data, size_t len) {
    // Simulate memcpy into queue slot
    volatile uint8_t sum = 0;
    for (size_t i = 0; i < len; i += 64) sum += data[i];  // touch each cache line

    // Simulate 2x release fence + atomic store (ARM: dmb ishst)
    std::atomic_thread_fence(std::memory_order_release);
    g_sink = reinterpret_cast<uint64_t>(data);
    std::atomic_thread_fence(std::memory_order_release);
    g_sink = len;
}

/// Extract symbol from bookTicker JSON payload.
/// Scans for "s":" pattern and extracts the value.
/// Returns empty string_view if not found.
static std::string_view extract_symbol_fast(const uint8_t* data, size_t len) {
    // bookTicker format: ..."s":"BTCUSDT"...
    // Scan for "s":" pattern — typically within first 80 bytes
    size_t scan_len = std::min(len, size_t{100});
    for (size_t i = 0; i + 4 < scan_len; ++i) {
        if (data[i] == '"' && data[i+1] == 's' && data[i+2] == '"' &&
            data[i+3] == ':' && data[i+4] == '"') {
            size_t start = i + 5;
            size_t end = start;
            while (end < len && data[end] != '"') ++end;
            return {reinterpret_cast<const char*>(data + start), end - start};
        }
    }
    return {};
}

// --- Strategy: Baseline (deliver every frame) ---
struct BaselineResult {
    size_t frames_delivered = 0;
};

static BaselineResult process_baseline(const uint8_t* data, size_t len) {
    BaselineResult result;
    size_t offset = 0;
    while (offset < len) {
        auto frame = eph::net::ws::decode_frame(data + offset, len - offset);
        if (!frame) break;
        offset += frame->total_len;
        if (frame->is_data() && frame->payload_len > 0) {
            simulate_enqueue(frame->payload, frame->payload_len);
            result.frames_delivered++;
        }
    }
    return result;
}

// --- Strategy A: Forward scan + reverse dedup (per-symbol last-only) ---
struct FrameIndex {
    size_t payload_offset;
    size_t payload_len;
    std::string_view symbol;
};

struct DedupeResult {
    size_t frames_scanned = 0;
    size_t frames_delivered = 0;
};

static DedupeResult process_reverse_dedup(const uint8_t* data, size_t len) {
    DedupeResult result;

    // Phase 1: Forward scan — build index with symbol extraction
    std::vector<FrameIndex> index;
    index.reserve(64);
    size_t offset = 0;
    while (offset < len) {
        auto frame = eph::net::ws::decode_frame(data + offset, len - offset);
        if (!frame) break;
        offset += frame->total_len;
        if (frame->is_data() && frame->payload_len > 0) {
            auto sym = extract_symbol_fast(frame->payload, frame->payload_len);
            index.push_back({
                static_cast<size_t>(frame->payload - data),
                frame->payload_len,
                sym
            });
            result.frames_scanned++;
        }
    }

    // Phase 2: Reverse iterate — deliver last per symbol
    // Use a small fixed array for seen symbols (max 64 symbols)
    std::array<std::string_view, 64> seen{};
    size_t seen_count = 0;

    for (auto it = index.rbegin(); it != index.rend(); ++it) {
        // Check if we've already seen this symbol
        bool found = false;
        for (size_t i = 0; i < seen_count; ++i) {
            if (seen[i] == it->symbol) { found = true; break; }
        }
        if (found) continue;

        // First time seeing this symbol (from the end) — deliver
        if (seen_count < seen.size()) seen[seen_count++] = it->symbol;
        simulate_enqueue(data + it->payload_offset, it->payload_len);
        result.frames_delivered++;
    }

    return result;
}

// --- Strategy B: Two-phase index + selective deliver ---
static DedupeResult process_two_phase(const uint8_t* data, size_t len) {
    DedupeResult result;

    // Phase 1: Forward scan — build index with symbol extraction
    // Same as Strategy A phase 1
    struct Entry {
        size_t payload_offset;
        size_t payload_len;
        std::string_view symbol;
    };

    // Use stack-allocated fixed array instead of vector for hot path
    std::array<Entry, 256> entries{};
    size_t entry_count = 0;

    size_t offset = 0;
    while (offset < len) {
        auto frame = eph::net::ws::decode_frame(data + offset, len - offset);
        if (!frame) break;
        offset += frame->total_len;
        if (frame->is_data() && frame->payload_len > 0 && entry_count < entries.size()) {
            auto sym = extract_symbol_fast(frame->payload, frame->payload_len);
            entries[entry_count++] = {
                static_cast<size_t>(frame->payload - data),
                frame->payload_len,
                sym
            };
            result.frames_scanned++;
        }
    }

    // Phase 2: Forward pass — track last index per symbol, then deliver only those
    // For each symbol, find the last occurrence
    std::array<std::string_view, 64> symbols{};
    std::array<size_t, 64> last_idx{};
    size_t sym_count = 0;

    for (size_t i = 0; i < entry_count; ++i) {
        bool found = false;
        for (size_t j = 0; j < sym_count; ++j) {
            if (symbols[j] == entries[i].symbol) {
                last_idx[j] = i;
                found = true;
                break;
            }
        }
        if (!found && sym_count < symbols.size()) {
            symbols[sym_count] = entries[i].symbol;
            last_idx[sym_count] = i;
            sym_count++;
        }
    }

    // Deliver last frame per symbol
    for (size_t j = 0; j < sym_count; ++j) {
        auto& e = entries[last_idx[j]];
        simulate_enqueue(data + e.payload_offset, e.payload_len);
        result.frames_delivered++;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Benchmark harness
// ---------------------------------------------------------------------------

struct BenchResult {
    double mean_ns;
    double min_ns;
    double p50_ns;
    double p99_ns;
    size_t frames_delivered;
};

template <typename F>
static BenchResult benchmark(const std::string& name, size_t iterations, F&& fn) {
    std::vector<double> samples;
    samples.reserve(iterations);

    size_t delivered = 0;

    // Warmup
    for (size_t i = 0; i < 100; ++i) {
        auto r = fn();
        delivered = r.frames_delivered;
    }

    // Measure
    for (size_t i = 0; i < iterations; ++i) {
        auto t0 = eph::utils::TSC::now();
        auto r = fn();
        auto t1 = eph::utils::TSC::now();
        delivered = r.frames_delivered;
        auto ns = eph::utils::TSC::to_ns(t1 - t0);
        if (ns) samples.push_back(static_cast<double>(*ns));
    }

    std::sort(samples.begin(), samples.end());
    double sum = 0;
    for (auto s : samples) sum += s;

    return {
        .mean_ns = sum / static_cast<double>(samples.size()),
        .min_ns = samples.front(),
        .p50_ns = samples[samples.size() / 2],
        .p99_ns = samples[samples.size() * 99 / 100],
        .frames_delivered = delivered,
    };
}

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    size_t n_frames = 30;   // typical burst size
    size_t iterations = 100000;

    if (argc > 1) n_frames = static_cast<size_t>(std::atoi(argv[1]));
    if (argc > 2) iterations = static_cast<size_t>(std::atoi(argv[2]));

    spdlog::info("Calibrating TSC...");
    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed"); return 1;
    }

    auto buf = build_ws_buffer(n_frames);
    spdlog::info("=== WS Frame Processing Micro-Benchmark ===");
    spdlog::info("Frames: {} ({} symbols) | Buffer: {} bytes | Iterations: {}",
                 n_frames, kSymbols.size(), buf.size(), iterations);
    spdlog::info("");

    // Verify all strategies produce correct results
    auto baseline = process_baseline(buf.data(), buf.size());
    auto rev = process_reverse_dedup(buf.data(), buf.size());
    auto two = process_two_phase(buf.data(), buf.size());

    spdlog::info("Verification:");
    spdlog::info("  Baseline:  {} frames delivered", baseline.frames_delivered);
    spdlog::info("  Reverse:   {} scanned, {} delivered", rev.frames_scanned, rev.frames_delivered);
    spdlog::info("  Two-phase: {} scanned, {} delivered", two.frames_scanned, two.frames_delivered);
    spdlog::info("");

    // Benchmark
    auto r_baseline = benchmark("Baseline", iterations, [&] {
        return BaselineResult{process_baseline(buf.data(), buf.size()).frames_delivered};
    });

    auto r_reverse = benchmark("Reverse dedup", iterations, [&] {
        auto r = process_reverse_dedup(buf.data(), buf.size());
        return BaselineResult{r.frames_delivered};
    });

    auto r_twophase = benchmark("Two-phase", iterations, [&] {
        auto r = process_two_phase(buf.data(), buf.size());
        return BaselineResult{r.frames_delivered};
    });

    // Report
    spdlog::info("=== Results ===");
    spdlog::info("");

    auto report = [](const std::string& name, const BenchResult& r) {
        spdlog::info("  {:<20} mean: {:>7.0f} ns | min: {:>7.0f} ns | p50: {:>7.0f} ns | p99: {:>7.0f} ns | delivered: {}",
                     name, r.mean_ns, r.min_ns, r.p50_ns, r.p99_ns, r.frames_delivered);
    };

    report("Baseline", r_baseline);
    report("Reverse dedup", r_reverse);
    report("Two-phase", r_twophase);

    spdlog::info("");
    spdlog::info("  Reverse vs Baseline:  {:.1f}x ({:+.0f} ns, {:+.1f}%)",
                 r_baseline.p50_ns / r_reverse.p50_ns,
                 r_reverse.p50_ns - r_baseline.p50_ns,
                 (r_reverse.p50_ns - r_baseline.p50_ns) / r_baseline.p50_ns * 100.0);
    spdlog::info("  Two-phase vs Baseline: {:.1f}x ({:+.0f} ns, {:+.1f}%)",
                 r_baseline.p50_ns / r_twophase.p50_ns,
                 r_twophase.p50_ns - r_baseline.p50_ns,
                 (r_twophase.p50_ns - r_baseline.p50_ns) / r_baseline.p50_ns * 100.0);

    // Also test with different frame counts
    spdlog::info("");
    spdlog::info("=== Scaling (p50, 3 symbols) ===");
    spdlog::info("  {:>6}  {:>12}  {:>12}  {:>12}", "Frames", "Baseline", "Reverse", "Two-phase");

    for (size_t nf : {3, 10, 30, 60, 100}) {
        auto b = build_ws_buffer(nf);
        auto rb = benchmark("", iterations / 10, [&] {
            return BaselineResult{process_baseline(b.data(), b.size()).frames_delivered};
        });
        auto rr = benchmark("", iterations / 10, [&] {
            auto r = process_reverse_dedup(b.data(), b.size());
            return BaselineResult{r.frames_delivered};
        });
        auto rt = benchmark("", iterations / 10, [&] {
            auto r = process_two_phase(b.data(), b.size());
            return BaselineResult{r.frames_delivered};
        });
        spdlog::info("  {:>6}  {:>9.0f} ns  {:>9.0f} ns  {:>9.0f} ns",
                     nf, rb.p50_ns, rr.p50_ns, rt.p50_ns);
    }

    return 0;
}
