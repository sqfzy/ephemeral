/// @file bench_single_vs_multi.cpp
/// A/B test: per-symbol connections (3) vs single combined connection (1).
///
/// Setup (4 connections total, all to the same resolved IP):
///   Per-symbol side:
///     Connection 1: /ws/btcusdt@bookTicker
///     Connection 2: /ws/ethusdt@bookTicker
///     Connection 3: /ws/solusdt@bookTicker
///   Combined side:
///     Connection 4: /stream?streams=btcusdt@bookTicker/ethusdt@bookTicker/solusdt@bookTicker
///
/// For each symbol independently, we pair events by "E" (event time ms)
/// and compute the TSC arrival difference: per-symbol TSC - combined TSC.
///   Negative = per-symbol arrived FIRST (faster)
///   Positive = combined arrived FIRST (faster)
///
/// Usage:
///   ./bench_single_vs_multi --duration 60

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <netdb.h>
#include <arpa/inet.h>

#include <spdlog/spdlog.h>

#include "eph/containers/evicting_queue.hpp"
#include "eph/net/socket_transport.hpp"
#include "eph/utils/time.hpp"

// Combined stream wraps payloads in {"stream":"...","data":{...}} — needs larger buffer.
// Burst aggregation can produce very large WS frames (observed up to ~9KB).
using TestTransport = eph::net::Transport<
    eph::net::SocketTransport,
    eph::net::WsFramer,
    16384, 256,
    eph::containers::EvictingQueue
>;

static const std::vector<std::string> kSymbols = {"btcusdt", "ethusdt", "solusdt"};

struct Config {
    std::string host    = "fstream.binance.com";
    uint16_t    port    = 443;
    int  duration       = 30;
};

static std::atomic<bool> g_running{true};
static void sig(int) { g_running.store(false, std::memory_order_release); }

static Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](std::string_view n) -> const char* {
            if (i + 1 >= argc) { std::cerr << std::format("{} requires a value\n", n); std::exit(1); }
            return argv[++i];
        };
        if      (a == "--host")     c.host     = next(a);
        else if (a == "--port")     c.port     = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--duration") c.duration = std::atoi(next(a));
        else if (a == "--help") {
            std::cerr << std::format("Usage: {} [--host H] [--port P] [--duration SEC]\n", argv[0]);
            std::exit(0);
        }
    }
    return c;
}

/// Extract the "E" (event time) field from bookTicker JSON.
static uint64_t extract_event_time(std::string_view json) {
    auto pos = json.find("\"E\":");
    if (pos == std::string_view::npos) return 0;
    pos += 4;
    while (pos < json.size() && json[pos] == ' ') ++pos;
    uint64_t val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + static_cast<uint64_t>(json[pos] - '0');
        ++pos;
    }
    return val;
}

/// Extract the "s" (symbol) field from bookTicker JSON.
/// Returns lowercase symbol string.
static std::string extract_symbol(std::string_view json) {
    auto pos = json.find("\"s\":\"");
    if (pos == std::string_view::npos) return {};
    pos += 5;
    auto end = json.find('"', pos);
    if (end == std::string_view::npos) return {};
    std::string sym(json.substr(pos, end - pos));
    // lowercase
    for (auto& c : sym) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return sym;
}

// Per-symbol event storage
struct EventRecord {
    uint64_t tsc_persymbol = 0;  // TSC from per-symbol connection
    uint64_t tsc_combined  = 0;  // TSC from combined connection
};

struct SymbolData {
    std::mutex mutex;
    std::map<uint64_t, EventRecord> events;  // event_time → record
};

// Global per-symbol data
static std::map<std::string, SymbolData> g_symbol_data;

static void record_arrival(const std::string& symbol, uint64_t event_time,
                           uint64_t tsc, bool is_persymbol) {
    auto it = g_symbol_data.find(symbol);
    if (it == g_symbol_data.end()) return;
    std::lock_guard lock(it->second.mutex);
    auto& rec = it->second.events[event_time];
    if (is_persymbol) {
        if (rec.tsc_persymbol == 0) rec.tsc_persymbol = tsc;
    } else {
        if (rec.tsc_combined == 0) rec.tsc_combined = tsc;
    }
}

/// Resolve hostname to a single IP.
static std::string resolve_host(const std::string& host) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
        return host;
    char buf[INET_ADDRSTRLEN]{};
    auto* sa = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
    inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
    freeaddrinfo(res);
    return buf;
}

static std::unique_ptr<TestTransport> create_transport(
    const std::string& ip, const Config& cfg,
    const std::string& ws_path, const std::string& label,
    std::function<void(const uint8_t*, uint16_t, uint8_t)> on_msg)
{
    eph::net::TransportConfig tc{
        .remote_host = cfg.host, .remote_port = cfg.port,
        .ws_path = ws_path, .use_tls = true, .verify_peer = false,
        .max_reconnect_attempts = 3,
        .ping_interval = std::chrono::seconds{0},
        .skip_utf8_validation = true,
        .on_state_change = [label](eph::net::TransportEvent e, std::string_view d) {
            spdlog::info("[{}] {} — {}", label, eph::net::transport_event_name(e), d);
        },
        .on_message = std::move(on_msg),
    };

    eph::net::SocketConfig sc{
        .host = ip, .port = cfg.port,
        .tcp_nodelay = true, .tcp_keepalive = true,
    };

    auto factory = [sc]() -> std::expected<std::unique_ptr<eph::net::SocketTransport>, std::string> {
        auto tcp = std::make_unique<eph::net::SocketTransport>(sc);
        if (auto r = tcp->connect(std::chrono::milliseconds{5000}); !r)
            return std::unexpected(r.error());
        return tcp;
    };

    auto result = TestTransport::create(std::move(factory), tc);
    if (!result) {
        spdlog::error("[{}] Connect failed: {}", label, result.error().message());
        return nullptr;
    }
    spdlog::info("[{}] Connected (handshake {:.2f} ms)", label,
                 (*result)->stats().handshake_ms());
    return std::move(*result);
}

struct PercentileStats {
    size_t samples = 0;
    double mean = 0;
    int64_t min = 0, p1 = 0, p25 = 0, p50 = 0, p75 = 0, p99 = 0, max = 0;
    size_t persymbol_faster = 0, combined_faster = 0, same = 0;
};

static PercentileStats compute_stats(std::vector<int64_t>& deltas) {
    if (deltas.empty()) return {};
    std::sort(deltas.begin(), deltas.end());

    auto pct = [&](double p) -> int64_t {
        size_t idx = static_cast<size_t>(p / 100.0 * static_cast<double>(deltas.size()));
        if (idx >= deltas.size()) idx = deltas.size() - 1;
        return deltas[idx];
    };

    PercentileStats s;
    s.samples = deltas.size();
    int64_t sum = 0;
    for (auto d : deltas) {
        sum += d;
        if (d < -1000) ++s.persymbol_faster;
        else if (d > 1000) ++s.combined_faster;
        else ++s.same;
    }
    s.mean = static_cast<double>(sum) / static_cast<double>(deltas.size());
    s.min = deltas.front();
    s.p1 = pct(1); s.p25 = pct(25); s.p50 = pct(50); s.p75 = pct(75); s.p99 = pct(99);
    s.max = deltas.back();
    return s;
}

static void print_symbol_stats(const std::string& symbol, SymbolData& sd) {
    std::lock_guard lock(sd.mutex);

    std::vector<int64_t> deltas;
    size_t persymbol_only = 0, combined_only = 0;

    for (auto& [et, rec] : sd.events) {
        if (rec.tsc_persymbol > 0 && rec.tsc_combined > 0) {
            int64_t tsc_diff = static_cast<int64_t>(rec.tsc_persymbol) -
                               static_cast<int64_t>(rec.tsc_combined);
            auto ns = eph::utils::TSC::to_ns(
                tsc_diff > 0 ? static_cast<uint64_t>(tsc_diff)
                             : static_cast<uint64_t>(-tsc_diff));
            if (ns) {
                deltas.push_back(tsc_diff > 0
                    ? static_cast<int64_t>(*ns)
                    : -static_cast<int64_t>(*ns));
            }
        } else if (rec.tsc_persymbol > 0) {
            ++persymbol_only;
        } else {
            ++combined_only;
        }
    }

    auto st = compute_stats(deltas);

    spdlog::info("--- {} ---", symbol);
    spdlog::info("  Paired: {} | PerSymbol-only: {} | Combined-only: {}",
                 st.samples, persymbol_only, combined_only);
    if (st.samples == 0) {
        spdlog::info("  (no paired events)");
        return;
    }
    spdlog::info("  Delta = per_symbol_arrival - combined_arrival (ns)");
    spdlog::info("    Negative = per-symbol connection arrived FIRST");
    spdlog::info("    Positive = combined connection arrived FIRST");
    spdlog::info("  mean:  {:.0f} ns", st.mean);
    spdlog::info("  min:   {} ns", st.min);
    spdlog::info("  p1:    {} ns", st.p1);
    spdlog::info("  p25:   {} ns", st.p25);
    spdlog::info("  p50:   {} ns", st.p50);
    spdlog::info("  p75:   {} ns", st.p75);
    spdlog::info("  p99:   {} ns", st.p99);
    spdlog::info("  max:   {} ns", st.max);
    spdlog::info("  Per-symbol faster (>1μs): {} ({:.1f}%)", st.persymbol_faster,
                 100.0 * static_cast<double>(st.persymbol_faster) / static_cast<double>(st.samples));
    spdlog::info("  Combined faster  (>1μs): {} ({:.1f}%)", st.combined_faster,
                 100.0 * static_cast<double>(st.combined_faster) / static_cast<double>(st.samples));
    spdlog::info("  Same (±1μs):             {} ({:.1f}%)", st.same,
                 100.0 * static_cast<double>(st.same) / static_cast<double>(st.samples));
}

int main(int argc, char** argv) {
    std::signal(SIGINT, sig);
    std::signal(SIGTERM, sig);
    spdlog::set_level(spdlog::level::info);

    auto cfg = parse_args(argc, argv);

    spdlog::info("Calibrating TSC...");
    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed"); return 1;
    }
    spdlog::info("TSC: {:.4f} ns/cycle", eph::utils::TSC::get_ns_per_cycle().value());

    // Initialize per-symbol data
    for (auto& sym : kSymbols) g_symbol_data[sym];

    // Build combined path
    std::string combined_streams;
    for (size_t i = 0; i < kSymbols.size(); ++i) {
        if (i > 0) combined_streams += '/';
        combined_streams += kSymbols[i] + "@bookTicker";
    }
    auto combined_path = "/stream?streams=" + combined_streams;

    auto resolved_ip = resolve_host(cfg.host);

    spdlog::info("=== Per-Symbol (3 conn) vs Combined (1 conn) A/B Test ===");
    spdlog::info("Symbols: {} | Duration: {}s", kSymbols.size(), cfg.duration);
    spdlog::info("Resolved {} → {}", cfg.host, resolved_ip);
    for (auto& sym : kSymbols)
        spdlog::info("  Per-symbol: /ws/{}@bookTicker", sym);
    spdlog::info("  Combined:   {}", combined_path);

    // ── Create per-symbol connections ──
    std::vector<std::unique_ptr<TestTransport>> per_symbol_tps;
    for (auto& sym : kSymbols) {
        auto path = std::format("/ws/{}@bookTicker", sym);
        auto label = std::format("PS:{}", sym);
        auto tp = create_transport(resolved_ip, cfg, path, label,
            [sym](const uint8_t* data, uint16_t len, uint8_t) {
                uint64_t tsc = eph::utils::TSC::now();
                std::string_view json(reinterpret_cast<const char*>(data), len);
                uint64_t et = extract_event_time(json);
                if (et > 0) record_arrival(sym, et, tsc, true);
            });
        if (!tp) return 1;
        per_symbol_tps.push_back(std::move(tp));
    }

    // ── Create combined connection ──
    auto tp_combined = create_transport(resolved_ip, cfg, combined_path, "COMBINED",
        [](const uint8_t* data, uint16_t len, uint8_t) {
            uint64_t tsc = eph::utils::TSC::now();
            std::string_view json(reinterpret_cast<const char*>(data), len);
            auto sym = extract_symbol(json);
            if (sym.empty()) return;
            uint64_t et = extract_event_time(json);
            if (et > 0) record_arrival(sym, et, tsc, false);
        });
    if (!tp_combined) return 1;

    spdlog::info("All {} connections established. Collecting data for {}s...",
                 kSymbols.size() + 1, cfg.duration);

    // ── Wait ──
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(cfg.duration);
    while (g_running.load(std::memory_order_acquire) &&
           tp_combined->is_running() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    // ── Stop all ──
    for (auto& tp : per_symbol_tps) tp->stop();
    tp_combined->stop();

    // ── Report per-symbol ──
    spdlog::info("");
    spdlog::info("=== Results (per-symbol) ===");
    for (auto& sym : kSymbols) {
        print_symbol_stats(sym, g_symbol_data[sym]);
    }

    return 0;
}
