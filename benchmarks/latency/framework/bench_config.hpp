/// @file framework/bench_config.hpp
/// Unified benchmark configuration and CLI parsing.
///
/// All scenarios share BenchConfig. Fields irrelevant to a particular
/// scenario are ignored (e.g. WS-specific fields in a raw UDP bench).

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

namespace bench {

// ── Default payload sizes per scenario category ──────────────────────────

inline const std::vector<size_t> kTcpPayloads = {64, 128, 256, 512, 1024, 1460};
inline const std::vector<size_t> kUdpPayloads = {64, 128, 512, 1024, 1472};
inline const std::vector<size_t> kWsPayloads  = {64, 128, 256, 512, 1024};

// ── BenchConfig ──────────────────────────────────────────────────────────

struct BenchConfig {
    // -- Common --
    std::string server_ip;
    uint16_t server_port = 9999;
    std::vector<size_t> payload_sizes;
    std::chrono::seconds warmup{2};
    std::chrono::seconds duration{10};
    int poll_cpu = 2;
    int mock_cpu = 4;
    std::string output_path;

    // -- WS-specific --
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    std::chrono::microseconds tick_interval{100};
    std::chrono::microseconds order_interval{1000};

    // -- DPDK-specific --
    std::string local_ip;
    std::string gateway_ip;
    uint16_t dpdk_port_id = 0;
};

namespace detail {

inline std::vector<std::string> split(std::string_view sv, char delim) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < sv.size()) {
        auto pos = sv.find(delim, start);
        if (pos == std::string_view::npos) pos = sv.size();
        if (pos > start) parts.emplace_back(sv.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

inline std::vector<size_t> parse_sizes(std::string_view sv) {
    std::vector<size_t> sizes;
    for (const auto& s : split(sv, ',')) {
        sizes.push_back(static_cast<size_t>(std::stoul(s)));
    }
    return sizes;
}

} // namespace detail

/// Parse CLI args into BenchConfig.
///
/// **Invariant**: This function loops over `argv[0..argc-1]` starting at
/// index 0 and matches entries against `--xxx` patterns. Program names
/// (e.g., `/path/to/bench_udp_echo`) never match a `--xxx` pattern, so
/// it is safe to pass either:
///
///   - main()'s argv directly: parse_bench_config(argc, argv)
///   - a slice (e.g., DPDK app_argv after `--`): parse_bench_config(app_argc, app_argv)
///
/// Unrecognized arguments are silently skipped (DPDK EAL args, program
/// names, etc.).
inline BenchConfig parse_bench_config(int argc, char** argv) {
    BenchConfig cfg;

    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];

        if      (arg == "--server-ip"      && i + 1 < argc) cfg.server_ip = argv[++i];
        else if (arg == "--port"           && i + 1 < argc) cfg.server_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--payload-sizes"  && i + 1 < argc) cfg.payload_sizes = detail::parse_sizes(argv[++i]);
        else if (arg == "--warmup"         && i + 1 < argc) cfg.warmup = std::chrono::seconds{std::stoi(argv[++i])};
        else if (arg == "--duration"       && i + 1 < argc) cfg.duration = std::chrono::seconds{std::stoi(argv[++i])};
        else if (arg == "--poll-cpu"       && i + 1 < argc) cfg.poll_cpu = std::stoi(argv[++i]);
        else if (arg == "--mock-cpu"       && i + 1 < argc) cfg.mock_cpu = std::stoi(argv[++i]);
        else if (arg == "--output"         && i + 1 < argc) cfg.output_path = argv[++i];
        else if (arg == "--symbols"        && i + 1 < argc) cfg.symbols = detail::split(argv[++i], ',');
        else if (arg == "--tick-interval"  && i + 1 < argc) cfg.tick_interval = std::chrono::microseconds{std::stoi(argv[++i])};
        else if (arg == "--order-interval" && i + 1 < argc) cfg.order_interval = std::chrono::microseconds{std::stoi(argv[++i])};
        else if (arg == "--local-ip"       && i + 1 < argc) cfg.local_ip = argv[++i];
        else if (arg == "--gateway-ip"     && i + 1 < argc) cfg.gateway_ip = argv[++i];
        else if (arg == "--dpdk-port"      && i + 1 < argc) cfg.dpdk_port_id = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--help" || arg == "-h") {
            spdlog::info(
                "Usage: <bench_binary> [--server-ip IP] [--port PORT]\n"
                "  [--payload-sizes 64,128,512] [--warmup SEC] [--duration SEC]\n"
                "  [--poll-cpu N] [--mock-cpu N] [--output FILE.jsonl]\n"
                "  [--symbols SYM1,SYM2] [--tick-interval US] [--order-interval US]\n"
                "  [--local-ip IP] [--gateway-ip IP] [--dpdk-port N]");
            std::exit(0);
        }
    }

    return cfg;
}

} // namespace bench
