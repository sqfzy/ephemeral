/// @file core/config.hpp
/// CLI / env-driven configuration types shared by every bench/mock binary.
///
/// `CommonConfig` is the only struct included by every binary. Scenario-
/// specific configs (`WsExchangeConfig`, `OrderConfig`, ...) live next
/// to their scenarios.
///
/// CLI > env > default. The env names are `BENCH_<UPPER_SNAKE>` of the
/// CLI name (e.g. `--client-cpu` ↔ `BENCH_CLIENT_CPU`).
///
/// Unrecognized argv entries are ignored — DPDK EAL args appear before
/// `--` and program names appear at index 0; both must pass through
/// silently.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bench {

struct CommonConfig {
    std::string server_ip;
    uint16_t    server_port = 0;
    std::chrono::seconds warmup{2};
    std::chrono::seconds duration{10};
    int  client_cpu = -1;          ///< bench client poll thread cpu
    int  mock_cpu   = -1;          ///< mock server thread cpu
    long server_work_ns = 0;       ///< spin N ns inside mock to model business work
    bool allow_non_isolated = false;
};

#ifdef EPH_USE_DPDK
struct DpdkConfig {
    std::string local_ip;
    std::string gateway_ip;
    std::string eal_cores = "0,1";
    uint16_t    dpdk_port_id = 0;
};
#endif

namespace config_detail {

inline std::optional<std::string> env(const char* name) {
    if (const char* v = std::getenv(name); v && *v) return std::string{v};
    return std::nullopt;
}

inline int parse_int(std::string_view s, int fallback) noexcept {
    int v = 0;
    bool any = false;
    bool neg = false;
    size_t i = 0;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        neg = (s[i] == '-');
        ++i;
    }
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return fallback;
        v = v * 10 + (s[i] - '0');
        any = true;
    }
    return any ? (neg ? -v : v) : fallback;
}

inline std::vector<std::string> split(std::string_view s, char delim) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        auto pos = s.find(delim, start);
        if (pos == std::string_view::npos) pos = s.size();
        if (pos > start) out.emplace_back(s.substr(start, pos - start));
        if (pos == s.size()) break;
        start = pos + 1;
    }
    return out;
}

inline std::vector<size_t> parse_size_list(std::string_view s) {
    std::vector<size_t> out;
    for (auto& p : split(s, ',')) {
        out.push_back(static_cast<size_t>(std::stoul(p)));
    }
    return out;
}

inline std::vector<int> parse_int_list(std::string_view s) {
    std::vector<int> out;
    for (auto& p : split(s, ',')) {
        out.push_back(std::stoi(p));
    }
    return out;
}

} // namespace config_detail

/// Parse common CLI flags from argv. Unrecognized flags are silently
/// skipped (program name, EAL args, scenario-specific flags).
///
/// Loop is index-driven: callers may pass either `argv` directly or a
/// post-`--` slice produced by DPDK EAL parsing.
inline CommonConfig parse_common(int argc, char** argv) {
    CommonConfig cfg;

    // Env defaults first.
    using namespace config_detail;
    if (auto v = env("BENCH_SERVER_IP"))   cfg.server_ip = *v;
    if (auto v = env("BENCH_SERVER_PORT")) cfg.server_port = static_cast<uint16_t>(parse_int(*v, 0));
    if (auto v = env("BENCH_WARMUP"))      cfg.warmup = std::chrono::seconds{parse_int(*v, 2)};
    if (auto v = env("BENCH_DURATION"))    cfg.duration = std::chrono::seconds{parse_int(*v, 10)};
    if (auto v = env("BENCH_CLIENT_CPU"))  cfg.client_cpu = parse_int(*v, -1);
    if (auto v = env("BENCH_MOCK_CPU"))    cfg.mock_cpu = parse_int(*v, -1);
    if (auto v = env("BENCH_SERVER_WORK_NS")) cfg.server_work_ns = parse_int(*v, 0);

    // CLI overrides.
    for (int i = 0; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](auto& field, auto convert) {
            if (i + 1 < argc) {
                field = convert(std::string_view{argv[++i]});
            }
        };
        if      (a == "--server-ip"           && i + 1 < argc) cfg.server_ip = argv[++i];
        else if (a == "--port"                && i + 1 < argc) cfg.server_port = static_cast<uint16_t>(parse_int(argv[++i], 0));
        else if (a == "--warmup"              && i + 1 < argc) cfg.warmup = std::chrono::seconds{parse_int(argv[++i], 2)};
        else if (a == "--duration"            && i + 1 < argc) cfg.duration = std::chrono::seconds{parse_int(argv[++i], 10)};
        else if (a == "--client-cpu"          && i + 1 < argc) cfg.client_cpu = parse_int(argv[++i], -1);
        else if (a == "--mock-cpu"            && i + 1 < argc) cfg.mock_cpu = parse_int(argv[++i], -1);
        else if (a == "--server-work-ns"      && i + 1 < argc) cfg.server_work_ns = parse_int(argv[++i], 0);
        else if (a == "--allow-non-isolated") cfg.allow_non_isolated = true;
        (void)next;
    }
    return cfg;
}

#ifdef EPH_USE_DPDK
/// Parse DPDK-specific flags from a post-`--` argv slice.
inline DpdkConfig parse_dpdk(int argc, char** argv) {
    DpdkConfig cfg;
    using namespace config_detail;
    if (auto v = env("BENCH_LOCAL_IP"))   cfg.local_ip = *v;
    if (auto v = env("BENCH_GATEWAY_IP")) cfg.gateway_ip = *v;
    if (auto v = env("BENCH_EAL_CORES"))  cfg.eal_cores = *v;
    if (auto v = env("BENCH_DPDK_PORT"))  cfg.dpdk_port_id = static_cast<uint16_t>(parse_int(*v, 0));

    for (int i = 0; i < argc; ++i) {
        std::string_view a = argv[i];
        if      (a == "--local-ip"   && i + 1 < argc) cfg.local_ip = argv[++i];
        else if (a == "--gateway-ip" && i + 1 < argc) cfg.gateway_ip = argv[++i];
        else if (a == "--eal-cores"  && i + 1 < argc) cfg.eal_cores = argv[++i];
        else if (a == "--dpdk-port"  && i + 1 < argc) cfg.dpdk_port_id = static_cast<uint16_t>(parse_int(argv[++i], 0));
    }
    return cfg;
}
#endif

} // namespace bench
