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

#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <unistd.h>

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

    /// Sweep axes. Empty = scenario falls back to its compiled-in default.
    /// Use `effective_payloads(cfg, fallback)` helper below to resolve.
    std::vector<size_t> payload_sizes;  ///< --payload-sizes 64,128,...
    std::vector<int>    inflights;      ///< --inflights 1,4,16,...
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

/// Read an environment variable and return its value if set and non-empty.
/// Returns `std::nullopt` otherwise — callers use this to decide whether to
/// fall back to a hard-coded default.
inline std::optional<std::string> env(const char* name) {
    if (const char* v = std::getenv(name); v && *v) return std::string{v};
    return std::nullopt;
}

/// Parse a decimal integer from `s`. Returns `fallback` on any parse error
/// (empty, non-digit characters). Accepts an optional leading `+`/`-`.
/// `noexcept` because the bench parses untrusted CLI input on the hot
/// boot path and must never throw.
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

/// Split `s` on `delim`. Adjacent delimiters collapse (empty fields are
/// dropped), which matches the "comma list" style used throughout bench.conf
/// (`TCP_PAYLOADS=64,128,256`).
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

/// Parse a comma-separated list of unsigned decimal integers into a vector.
/// Used for payload sweep axes (`64,128,256,...`).
inline std::vector<size_t> parse_size_list(std::string_view s) {
    std::vector<size_t> out;
    for (auto& p : split(s, ',')) {
        out.push_back(static_cast<size_t>(std::stoul(p)));
    }
    return out;
}

/// Parse a comma-separated list of signed decimal integers. Used for the
/// `INFLIGHTS=1,4,16,64` sweep axis consumed by `lat_ex_order`.
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
    if (auto v = env("BENCH_PAYLOAD_SIZES")) cfg.payload_sizes = parse_size_list(*v);
    if (auto v = env("BENCH_INFLIGHTS"))   cfg.inflights = parse_int_list(*v);

    // CLI overrides.
    for (int i = 0; i < argc; ++i) {
        std::string_view a = argv[i];
        if      (a == "--server-ip"           && i + 1 < argc) cfg.server_ip = argv[++i];
        else if (a == "--port"                && i + 1 < argc) cfg.server_port = static_cast<uint16_t>(parse_int(argv[++i], 0));
        else if (a == "--warmup"              && i + 1 < argc) cfg.warmup = std::chrono::seconds{parse_int(argv[++i], 2)};
        else if (a == "--duration"            && i + 1 < argc) cfg.duration = std::chrono::seconds{parse_int(argv[++i], 10)};
        else if (a == "--client-cpu"          && i + 1 < argc) cfg.client_cpu = parse_int(argv[++i], -1);
        else if (a == "--mock-cpu"            && i + 1 < argc) cfg.mock_cpu = parse_int(argv[++i], -1);
        else if (a == "--server-work-ns"      && i + 1 < argc) cfg.server_work_ns = parse_int(argv[++i], 0);
        else if (a == "--payload-sizes"       && i + 1 < argc) cfg.payload_sizes = parse_size_list(argv[++i]);
        else if (a == "--inflights"           && i + 1 < argc) cfg.inflights = parse_int_list(argv[++i]);
        else if (a == "--allow-non-isolated") cfg.allow_non_isolated = true;
    }
    return cfg;
}

/// Resolve the sweep axis for RTT scenarios. Returns `cfg.payload_sizes`
/// if the user passed `--payload-sizes`, otherwise returns the compiled-in
/// fallback unchanged.
inline std::span<const size_t>
effective_payloads(const CommonConfig& cfg,
                   std::span<const size_t> fallback) noexcept {
    return cfg.payload_sizes.empty()
        ? fallback
        : std::span<const size_t>{cfg.payload_sizes};
}

/// Resolve the sweep axis for the exchange/order scenario.
inline std::span<const int>
effective_inflights(const CommonConfig& cfg,
                    std::span<const int> fallback) noexcept {
    return cfg.inflights.empty()
        ? fallback
        : std::span<const int>{cfg.inflights};
}

// ─── BenchConfig (single-source config from bench.conf) ─────────────────
//
// Stage 3 of the simplify plan. The legacy `parse_common` API stays for
// the still-CLI-driven stage-2 binaries; new binaries (stage 4-5) read
// `BenchConfig` directly via `load_bench_conf()`.

/// Single struct holding every field a latency benchmark binary may need.
/// Each binary reads only the fields it cares about; unused fields are
/// silently kept at their defaults.
struct BenchConfig {
    // --- Networking ---
    std::string nic_a;            ///< NIC_A (informational, for log)
    std::string nic_b;            ///< NIC_B (informational, for log)
    std::string server_ip;        ///< mock bind address
    std::string local_ip;         ///< client local IP (DPDK needs it)
    std::string gateway_ip;       ///< NIC-B default gateway

    // --- CPU pinning ---
    // Defaults are -1 (sentinel) so `load_bench_conf` can detect that
    // CLIENT_CPU / MOCK_CPU were not set in bench.conf and raise an
    // error. The plan calls them required fields.
    int  client_cpu = -1;
    int  mock_cpu   = -1;
    std::string eal_cores = "0,1";
    bool allow_non_isolated = false;

    // --- Measurement window ---
    std::chrono::seconds warmup{2};
    std::chrono::seconds duration{10};
    long server_work_ns = 200;

    // --- Payload sweeps ---
    std::vector<size_t> tcp_payloads;
    std::vector<size_t> udp_payloads;
    std::vector<size_t> ws_payloads;
    std::vector<size_t> md_udp_payloads;
    std::vector<int>    inflights;

    // --- Exchange mock tuning ---
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    long   bookticker_us = 1000;
    long   depth_ms      = 10;
    long   trade_mean_ms = 5;
    long   kline_s       = 1;
    size_t depth_bytes   = 1024;

    // --- Server TCP listen port (used by stage-4+ scenarios) ---
    uint16_t server_port = 9000;

    // --- DPDK RSS / multi-queue configuration ---
    // Used by the multi-queue lat_*_dpdk variants to drive per-lcore
    // Pollers. Default 1 / false keeps every existing scenario in
    // single-queue Software mode (zero behavioural change).
    //
    // When NB_RX_QUEUES > 1 + ENABLE_RSS=true:
    //   * Platform::create installs an RSS hash + RETA across NB_RX_QUEUES.
    //   * The lat scenario spawns one DpdkPoller per queue and pins it to
    //     the corresponding lcore listed in LCORE_PER_QUEUE (CSV indexed
    //     by qid). The lcore CSV must list at least NB_RX_QUEUES values.
    //   * Each connection / stream calls DpdkTcpStream::create_and_attach
    //     with pin_to_queue set to its qid (round-robin across symbols).
    uint16_t    nb_rx_queues  = 1;
    bool        enable_rss    = false;
    std::string lcore_per_queue;     ///< CSV like "4,5,6,7" — indexed by qid
};

namespace config_detail {

/// Trim ASCII whitespace from both ends of `s` and return the result as a
/// freshly-owned string.
inline std::string strip(std::string_view s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string{s.substr(b, e - b)};
}

/// Strip surrounding single or double quotes (bash-style).
inline std::string unquote(std::string s) {
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

/// Parse a single bash-style KEY=VALUE line. Trailing `# comment` is
/// stripped (only when `#` is preceded by whitespace, to allow `#` inside
/// quoted values). Returns false on syntactic error.
inline bool parse_kv_line(std::string_view line,
                          std::string* key, std::string* val) {
    auto l = strip(line);
    if (l.empty() || l[0] == '#') return false;
    auto eq = l.find('=');
    if (eq == std::string::npos) return false;
    *key = strip(std::string_view{l}.substr(0, eq));
    auto rest = std::string_view{l}.substr(eq + 1);
    // Strip trailing inline comment ` #...` (but keep `#` inside quotes).
    bool in_squote = false, in_dquote = false;
    for (size_t i = 0; i < rest.size(); ++i) {
        char c = rest[i];
        if (c == '\'' && !in_dquote) in_squote = !in_squote;
        else if (c == '"' && !in_squote) in_dquote = !in_dquote;
        else if (c == '#' && !in_squote && !in_dquote &&
                 i > 0 && std::isspace(static_cast<unsigned char>(rest[i - 1]))) {
            rest = rest.substr(0, i);
            break;
        }
    }
    *val = unquote(strip(rest));
    return true;
}

/// Locate `bench.conf`. Order:
///   1. $BENCH_CONFIG (absolute path)
///   2. ./bench.conf in cwd
///   3. walk up from /proc/self/exe to find benchmarks/latency/bench.conf
inline std::optional<std::filesystem::path> find_bench_conf() {
    namespace fs = std::filesystem;
    if (auto v = env("BENCH_CONFIG")) {
        fs::path p{*v};
        if (fs::exists(p)) return p;
        return std::nullopt;
    }
    if (fs::path cwd = "bench.conf"; fs::exists(cwd)) return cwd;

    // Walk up from /proc/self/exe.
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        fs::path exe{buf};
        for (fs::path d = exe.parent_path(); !d.empty() && d != d.root_path();
             d = d.parent_path()) {
            fs::path candidate = d / "benchmarks" / "latency" / "bench.conf";
            if (fs::exists(candidate)) return candidate;
        }
    }
    return std::nullopt;
}

/// Apply a single KEY=VALUE pair from `bench.conf` to `cfg`. Unknown keys
/// are silently ignored so older binaries tolerate newer config files.
inline void apply_kv(BenchConfig& cfg, const std::string& k,
                     const std::string& v) {
    if      (k == "NIC_A")           cfg.nic_a = v;
    else if (k == "NIC_B")           cfg.nic_b = v;
    else if (k == "SERVER_IP")       cfg.server_ip = v;
    else if (k == "LOCAL_IP")        cfg.local_ip = v;
    else if (k == "GATEWAY_IP")      cfg.gateway_ip = v;
    else if (k == "CLIENT_CPU")      cfg.client_cpu = parse_int(v, -1);
    else if (k == "MOCK_CPU")        cfg.mock_cpu = parse_int(v, -1);
    else if (k == "EAL_CORES")       cfg.eal_cores = v;
    else if (k == "ALLOW_NON_ISOLATED") {
        cfg.allow_non_isolated = (v == "true" || v == "1" || v == "yes");
    }
    else if (k == "WARMUP")          cfg.warmup = std::chrono::seconds{parse_int(v, 2)};
    else if (k == "DURATION")        cfg.duration = std::chrono::seconds{parse_int(v, 10)};
    else if (k == "SERVER_WORK_NS")  cfg.server_work_ns = parse_int(v, 200);
    else if (k == "SERVER_PORT")     cfg.server_port = static_cast<uint16_t>(parse_int(v, 9000));
    else if (k == "TCP_PAYLOADS")    cfg.tcp_payloads = parse_size_list(v);
    else if (k == "UDP_PAYLOADS")    cfg.udp_payloads = parse_size_list(v);
    else if (k == "WS_PAYLOADS")     cfg.ws_payloads = parse_size_list(v);
    else if (k == "MD_UDP_PAYLOADS") cfg.md_udp_payloads = parse_size_list(v);
    else if (k == "INFLIGHTS")       cfg.inflights = parse_int_list(v);
    else if (k == "SYMBOLS") {
        cfg.symbols.clear();
        for (auto& s : split(v, ',')) cfg.symbols.push_back(s);
    }
    else if (k == "BOOKTICKER_US")   cfg.bookticker_us = parse_int(v, 1000);
    else if (k == "DEPTH_MS")        cfg.depth_ms = parse_int(v, 10);
    else if (k == "TRADE_MEAN_MS")   cfg.trade_mean_ms = parse_int(v, 5);
    else if (k == "KLINE_S")         cfg.kline_s = parse_int(v, 1);
    else if (k == "DEPTH_BYTES")     cfg.depth_bytes = static_cast<size_t>(parse_int(v, 1024));
    else if (k == "NB_RX_QUEUES")    cfg.nb_rx_queues = static_cast<uint16_t>(parse_int(v, 1));
    else if (k == "ENABLE_RSS") {
        cfg.enable_rss = (v == "true" || v == "1" || v == "yes" || v == "on");
    }
    else if (k == "LCORE_PER_QUEUE") cfg.lcore_per_queue = v;
    // Unknown keys are silently ignored — bench.conf may grow new keys
    // that older binaries don't understand.
}

} // namespace config_detail

/// Read `bench.conf` (bash-style KEY=VALUE) and return a populated
/// BenchConfig. Lookup order: $BENCH_CONFIG → ./bench.conf → walk up from
/// /proc/self/exe to <project>/benchmarks/latency/bench.conf.
///
/// Returns an error if no config file is found or any required field is
/// missing. Required: NIC_B, SERVER_IP, LOCAL_IP, GATEWAY_IP, CLIENT_CPU,
/// MOCK_CPU.
[[nodiscard]] inline std::expected<BenchConfig, std::string>
load_bench_conf() {
    using namespace config_detail;
    auto path = find_bench_conf();
    if (!path) {
        return std::unexpected(
            "bench.conf not found (checked $BENCH_CONFIG, ./bench.conf, "
            "and walking up from /proc/self/exe)");
    }

    std::ifstream in{*path};
    if (!in) {
        return std::unexpected("failed to open bench.conf at " + path->string());
    }

    BenchConfig cfg;
    std::string line, key, val;
    size_t lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (parse_kv_line(line, &key, &val)) {
            apply_kv(cfg, key, val);
        }
    }

    // Required fields.
    if (cfg.nic_b.empty())      return std::unexpected("bench.conf: NIC_B is required");
    if (cfg.server_ip.empty())  return std::unexpected("bench.conf: SERVER_IP is required");
    if (cfg.local_ip.empty())   return std::unexpected("bench.conf: LOCAL_IP is required");
    if (cfg.gateway_ip.empty()) return std::unexpected("bench.conf: GATEWAY_IP is required");
    if (cfg.client_cpu < 0)     return std::unexpected("bench.conf: CLIENT_CPU is required");
    if (cfg.mock_cpu < 0)       return std::unexpected("bench.conf: MOCK_CPU is required");

    return cfg;
}

// ─── ScenarioConfig (INI [lat_*] section parser) ────────────────────────
//
// Per-scenario INI sections in bench.conf. Each
// scenario binary reads the global `CommonConfig` / `BenchConfig` values
// for NIC/IP/CPU layout AND its own `[lat_<name>]` section for port,
// payload size, duration, etc.
//
// Rationale (D-1, D-2 in plan-phase-10-latency-bench-20260411-040540.md):
// one flat-structure bench.conf keeps config local to the scenario it
// drives while sharing the global layout. The old bash-style KEY=VALUE
// format stays backwards-compatible for lines outside any `[...]` header.

/// Parse a `[lat_<name>]` INI section from bench.conf into a key-value
/// map. Global (pre-section) lines are ignored — use `CommonConfig` or
/// `BenchConfig` for those.
///
/// Scenario binaries typically do:
/// ```cpp
/// auto scenario = bench::ScenarioConfig::load("bench.conf", "lat_tcp")
///                    .value();
/// auto port = scenario.get_u32("port").value();
/// auto payload = scenario.get_u32("payload_size", 256);
/// ```
///
/// Parser constraints:
///  - Lines before the first `[...]` header are silently ignored (global
///    keys belong to CommonConfig / BenchConfig).
///  - Blank lines and `#`-prefixed comment lines are skipped.
///  - Lines inside a `[<section>]` block are collected only when
///    `<section>` matches `scenario_name` exactly (no prefix match, so
///    `lat_tcp` does not leak into `lat_tcp_variant`).
///  - `key = value`: both sides trimmed; internal whitespace preserved.
///  - A missing section returns an empty ScenarioConfig (NOT an error).
///    Required-key validation happens at the get_*() call site.
class ScenarioConfig {
public:
    /// Load the global (pre-section) `key = value` lines from `conf_path`.
    ///
    /// bench.conf has lowercase global keys (`mock_ip`,
    /// `client_ip`, `warmup_samples`, ...) BEFORE the first `[lat_*]`
    /// section header. They are shared by every scenario binary and the
    /// Python mocks, so each scenario reads them via this helper:
    /// ```cpp
    /// auto globals = ScenarioConfig::load_globals(path).value();
    /// auto mock_ip = globals.get_string("mock_ip");
    /// auto warmup  = globals.get_u32("warmup_samples", 1000).value();
    /// ```
    ///
    /// Lines inside any `[...]` block are ignored — those belong to the
    /// scenario-specific `load(conf_path, "lat_foo")` call.
    [[nodiscard]] static std::expected<ScenarioConfig, std::string>
    load_globals(std::string_view conf_path) {
        ScenarioConfig out;
        std::ifstream in{std::string{conf_path}};
        if (!in) {
            return std::unexpected(
                "ScenarioConfig: failed to open " + std::string{conf_path});
        }

        std::string line;
        size_t lineno = 0;
        while (std::getline(in, line)) {
            ++lineno;
            auto trimmed = config_detail::strip(line);
            if (trimmed.empty() || trimmed[0] == '#') continue;

            // Stop at the first `[...]` section header — globals are
            // whatever comes before it.
            if (trimmed.front() == '[') break;

            // Strict `key = value` with no implicit defaults.
            auto eq = trimmed.find('=');
            if (eq == std::string::npos) {
                // Tolerate stray non-KV lines: the legacy bash-style
                // parser (`BenchConfig`) used the same file and may leave
                // comment-like lines we should just skip.
                continue;
            }
            auto key = config_detail::strip(
                std::string_view{trimmed}.substr(0, eq));
            auto val = config_detail::strip(
                std::string_view{trimmed}.substr(eq + 1));
            if (key.empty()) continue;
            out.kv_[key] = val;
        }
        return out;
    }

    /// Load a section from `conf_path`. Returns Ok (possibly empty) on
    /// success, Err only on I/O or syntactic error (e.g. missing `=`).
    [[nodiscard]] static std::expected<ScenarioConfig, std::string>
    load(std::string_view conf_path, std::string_view scenario_name) {
        ScenarioConfig out;
        std::ifstream in{std::string{conf_path}};
        if (!in) {
            return std::unexpected(
                "ScenarioConfig: failed to open " + std::string{conf_path});
        }

        const std::string target{scenario_name};
        std::string line;
        bool in_target = false;
        size_t lineno = 0;
        while (std::getline(in, line)) {
            ++lineno;
            auto trimmed = config_detail::strip(line);
            if (trimmed.empty() || trimmed[0] == '#') continue;

            // Section header?
            if (trimmed.front() == '[') {
                auto rb = trimmed.find(']');
                if (rb == std::string::npos) {
                    return std::unexpected(
                        "ScenarioConfig: malformed section header at line " +
                        std::to_string(lineno) + ": " + trimmed);
                }
                auto name = config_detail::strip(
                    std::string_view{trimmed}.substr(1, rb - 1));
                in_target = (name == target);
                continue;
            }

            if (!in_target) continue;

            // key = value line. Do NOT use parse_kv_line — it strips
            // trailing `# comment` conditionally and unquotes; ScenarioConfig
            // wants raw-ish parsing with strict error reporting.
            auto eq = trimmed.find('=');
            if (eq == std::string::npos) {
                return std::unexpected(
                    "ScenarioConfig: missing '=' at line " +
                    std::to_string(lineno) + ": " + trimmed);
            }
            auto key = config_detail::strip(
                std::string_view{trimmed}.substr(0, eq));
            auto val = config_detail::strip(
                std::string_view{trimmed}.substr(eq + 1));
            if (key.empty()) {
                return std::unexpected(
                    "ScenarioConfig: empty key at line " +
                    std::to_string(lineno));
            }
            out.kv_[key] = val;
        }
        return out;
    }

    /// String getter with default.
    [[nodiscard]] std::string
    get_string(std::string_view key, std::string_view def = "") const {
        auto it = kv_.find(std::string{key});
        if (it == kv_.end()) return std::string{def};
        return it->second;
    }

    /// Required u32: error if missing or unparseable.
    [[nodiscard]] std::expected<uint32_t, std::string>
    get_u32(std::string_view key) const {
        auto it = kv_.find(std::string{key});
        if (it == kv_.end()) {
            return std::unexpected(
                "ScenarioConfig: missing required key '" +
                std::string{key} + "'");
        }
        return parse_u32(it->second, key);
    }

    /// Optional u32 with default: missing key returns default, but a
    /// present-but-malformed value is still an error (fail-loud).
    [[nodiscard]] std::expected<uint32_t, std::string>
    get_u32(std::string_view key, uint32_t def) const {
        auto it = kv_.find(std::string{key});
        if (it == kv_.end()) return def;
        return parse_u32(it->second, key);
    }

    [[nodiscard]] std::expected<uint64_t, std::string>
    get_u64(std::string_view key) const {
        auto it = kv_.find(std::string{key});
        if (it == kv_.end()) {
            return std::unexpected(
                "ScenarioConfig: missing required key '" +
                std::string{key} + "'");
        }
        return parse_u64(it->second, key);
    }

    [[nodiscard]] std::expected<uint64_t, std::string>
    get_u64(std::string_view key, uint64_t def) const {
        auto it = kv_.find(std::string{key});
        if (it == kv_.end()) return def;
        return parse_u64(it->second, key);
    }

    [[nodiscard]] std::expected<double, std::string>
    get_double(std::string_view key) const {
        auto it = kv_.find(std::string{key});
        if (it == kv_.end()) {
            return std::unexpected(
                "ScenarioConfig: missing required key '" +
                std::string{key} + "'");
        }
        // strtod is the least-bad stdlib option; errno must be reset
        // per POSIX. We reject trailing garbage.
        errno = 0;
        const char* begin = it->second.c_str();
        char* end = nullptr;
        double v = std::strtod(begin, &end);
        if (end == begin || *end != '\0' || errno == ERANGE) {
            return std::unexpected(
                "ScenarioConfig: key '" + std::string{key} +
                "' value '" + it->second + "' is not a valid double");
        }
        return v;
    }

    /// True if the key is present (regardless of whether its value is
    /// parseable). Useful for optional features that are "enabled when
    /// key exists".
    [[nodiscard]] bool has(std::string_view key) const {
        return kv_.find(std::string{key}) != kv_.end();
    }

    /// Number of keys parsed from the section. Primarily a test helper —
    /// production code should call has()/get_*() directly.
    [[nodiscard]] size_t size() const noexcept { return kv_.size(); }

private:
    /// Strict unsigned decimal parser. Rejects leading `-`, `+`, any
    /// non-digit, empty strings. Detects u32 overflow.
    [[nodiscard]] static std::expected<uint32_t, std::string>
    parse_u32(std::string_view s, std::string_view key) {
        if (s.empty()) {
            return std::unexpected(
                "ScenarioConfig: key '" + std::string{key} +
                "' has empty value");
        }
        uint64_t v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') {
                return std::unexpected(
                    "ScenarioConfig: key '" + std::string{key} +
                    "' value '" + std::string{s} + "' is not a u32");
            }
            v = v * 10 + uint64_t(c - '0');
            if (v > 0xFFFFFFFFull) {
                return std::unexpected(
                    "ScenarioConfig: key '" + std::string{key} +
                    "' value '" + std::string{s} + "' overflows u32");
            }
        }
        return static_cast<uint32_t>(v);
    }

    /// Strict unsigned decimal parser for u64.
    [[nodiscard]] static std::expected<uint64_t, std::string>
    parse_u64(std::string_view s, std::string_view key) {
        if (s.empty()) {
            return std::unexpected(
                "ScenarioConfig: key '" + std::string{key} +
                "' has empty value");
        }
        uint64_t v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') {
                return std::unexpected(
                    "ScenarioConfig: key '" + std::string{key} +
                    "' value '" + std::string{s} + "' is not a u64");
            }
            // Overflow check: if v > (UINT64_MAX - digit) / 10 then overflow.
            uint64_t digit = uint64_t(c - '0');
            if (v > (UINT64_MAX - digit) / 10) {
                return std::unexpected(
                    "ScenarioConfig: key '" + std::string{key} +
                    "' value '" + std::string{s} + "' overflows u64");
            }
            v = v * 10 + digit;
        }
        return v;
    }

    std::unordered_map<std::string, std::string> kv_;
};

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
