/// @file core/bench_conf.hpp
/// TOML-backed bench configuration loader.
///
/// Header-only wrapper around toml++ that parses `config.toml` (the
/// machine + scenario layout for the lat suite) into a structured
/// `BenchConfig` with strong typing, plus a generic `load_toml_table`
/// helper for auxiliary TOML files (MMPP params etc.).
///
/// Replaces the three overlapping parsers that used to live in
/// `benchmarks/latency/core/config.hpp` (CommonConfig / BenchConfig-legacy
/// / ScenarioConfig). See `.claude/plans/prancy-inventing-sky.md` for the
/// reshape plan.
///
/// ## Usage
///
/// @code
///   auto cfg_r = bench::load_bench_conf("benchmarks/latency/config.toml");
///   if (!cfg_r) {
///       std::fprintf(stderr, "%s\n",
///                    bench::format_error(cfg_r.error()).c_str());
///       return 1;
///   }
///   const auto& cfg = *cfg_r;
///
///   const int cpu = cfg.cpu.cpu_client;
///   const std::string& server_ip = cfg.networking.server_ip;
///
///   const bench::Scenario* tcp = cfg.scenario("lat_tcp");
///   const uint16_t port = tcp->get<uint16_t>("port").value();
///   const uint64_t dur  = tcp->get_or<uint64_t>("duration_seconds", 30);
/// @endcode
///
/// ## Error Handling
///
/// All failure modes collapse to `ConfigErrorInfo` (enum code + key path
/// + human-readable detail + file:line). Tests compare `.error().code`
/// against the enum; operators see `format_error(err)` output.

#pragma once

#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <toml++/toml.hpp>

namespace bench {

// ─── Error types ───────────────────────────────────────────────────────

enum class ConfigError {
    FileNotFound,         ///< open(path) failed / path empty
    ParseFailed,          ///< toml++ reported syntax error
    MissingRequiredKey,   ///< required field absent
    WrongType,            ///< key present but wrong TOML type
    InvalidValue,         ///< value out of range / empty when must be non-empty
};

struct ConfigErrorInfo {
    ConfigError code;
    std::string key;      ///< dotted TOML path, e.g. "cpu.cpu_client"
    std::string detail;   ///< human prose
    std::string file;     ///< config file path (best-effort)
    int         line;     ///< 1-based line number from toml++ when known; 0 otherwise
};

/// Format a ConfigErrorInfo into a one-line operator-friendly message.
[[nodiscard]] inline std::string format_error(const ConfigErrorInfo& err) {
    const char* code_name = [&] {
        switch (err.code) {
            case ConfigError::FileNotFound:       return "file-not-found";
            case ConfigError::ParseFailed:        return "parse-failed";
            case ConfigError::MissingRequiredKey: return "missing-required-key";
            case ConfigError::WrongType:          return "wrong-type";
            case ConfigError::InvalidValue:       return "invalid-value";
        }
        return "unknown";
    }();
    std::string out = "config ";
    out.append(code_name);
    if (!err.key.empty()) {
        out.append(" [").append(err.key).append("]");
    }
    if (!err.file.empty()) {
        out.append(" (").append(err.file);
        if (err.line > 0) {
            out.append(":").append(std::to_string(err.line));
        }
        out.append(")");
    }
    if (!err.detail.empty()) {
        out.append(": ").append(err.detail);
    }
    return out;
}

// ─── Structured config POD types ───────────────────────────────────────

struct Networking {
    std::string nic_a;         ///< server-side NIC (kernel mock binds here)
    std::string nic_b;         ///< client-side NIC (kernel netns or vfio-pci)
    std::string server_ip;     ///< NIC_A IP (required)
    std::string client_ip;     ///< NIC_B IP (required)
    std::string gateway_ip;    ///< default gateway that answers ARP (required)
    std::string netmask;       ///< e.g. "255.255.240.0"
    std::string nic_b_pci;     ///< optional, for vfio-pci scenarios
};

struct Cpu {
    int  cpu_client = -1;      ///< required
    int  cpu_mock   = -1;      ///< required
    std::string eal_cores = "0,1";
    bool allow_non_isolated = false;
};

struct Measurement {
    uint32_t warmup_samples = 1000;
    uint32_t server_work_ns = 200;
};

struct DpdkRss {
    uint16_t nb_rx_queues = 1;
    std::string lcore_per_queue;   ///< CSV; reserved (not yet consumed)
};

/// One row of the `[parallel].runs[]` array. Each row maps a single
/// lat scenario to the EAL lcore + CPU + RX queue it should run on
/// inside `lat_multi_dpdk` (the parallel-bench v2 runner). Multiple
/// rows with the same `scenario` are allowed (run N copies of the
/// same scenario on different queues for fan-out load testing).
struct RunSpec {
    std::string scenario;       ///< "lat_tcp" / "lat_udp" / ...
    uint16_t    lcore = 0;      ///< EAL lcore id (passed via LcorePin)
    int         cpu   = -1;     ///< CPU id this lcore pins to (typed)
    uint16_t    queue = 0;      ///< RX queue this run owns
};

/// `[parallel]` section — drives `lat all --dpdk` (multi-scenario
/// single-process N-lcore runner via `lat_multi_dpdk`). Single-
/// scenario commands (`lat tcp --dpdk` etc.) ignore this section
/// entirely.
///
/// **Parallel-bench v2 schema (this file)**:
/// `[parallel].runs = [{scenario=..., lcore=N, cpu=M, queue=Q}, ...]`
/// with one inline-table per concurrent run.
///
/// The dispatcher computes `nb_rx_queues = max(queue) + 1`,
/// `LcorePin[]` from `(lcore, cpu)` pairs, and exec's
/// `lat_multi_dpdk` which brings up a single EAL session sharing the
/// NIC across all runs (`Platform::create_with_eal` + per-queue
/// `register_poller` + `rte_eal_remote_launch` per worker). RSS routes
/// each run's reverse traffic to its owned queue via `pin_to_queue`.
struct ParallelConfig {
    std::vector<RunSpec> runs;
};

// ─── Internal helpers ─────────────────────────────────────────────────

namespace detail {

/// Extract a file:line pair from a toml++ source_region if possible.
inline int region_line(const toml::source_region& r) noexcept {
    return static_cast<int>(r.begin.line);
}

/// Build a ConfigErrorInfo for a missing key.
inline ConfigErrorInfo make_missing(std::string_view key,
                                    std::string_view file = "",
                                    int line = 0) {
    return {ConfigError::MissingRequiredKey,
            std::string{key},
            std::format("required TOML key '{}' is absent", key),
            std::string{file},
            line};
}

/// Build a ConfigErrorInfo for a wrong-type key.
inline ConfigErrorInfo make_wrong_type(std::string_view key,
                                       std::string_view expected,
                                       std::string_view file = "",
                                       int line = 0) {
    return {ConfigError::WrongType,
            std::string{key},
            std::format("TOML key '{}' has wrong type (expected {})",
                        key, expected),
            std::string{file},
            line};
}

} // namespace detail

// ─── Scenario: non-owning view over one [scenarios.<name>] subtable ───

/// Typed accessor for a `[scenarios.<name>]` TOML subtable.
///
/// Holds a non-owning pointer into the BenchConfig's owned `toml::table`.
/// Must not outlive the BenchConfig it came from — BenchConfig is
/// move-only and exposes Scenario by const reference to prevent misuse.
class Scenario {
public:
    /// Default-constructed: invalid sentinel; `has(...)` / `get<T>(...)` all fail.
    Scenario() noexcept = default;

    /// Construct over a live `toml::table` and a human name (owned).
    Scenario(const toml::table* tbl, std::string name) noexcept
        : tbl_(tbl), name_(std::move(name)) {}

    /// Scenario identifier, e.g. "lat_tcp". Empty iff default-constructed.
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

    /// True iff `key` is present in the underlying subtable.
    [[nodiscard]] bool has(std::string_view key) const noexcept {
        return tbl_ != nullptr && tbl_->contains(key);
    }

    /// Required-get: returns the value of `key` typed as `T` or a
    /// ConfigErrorInfo describing what went wrong. Supports:
    ///   - integral T (uint16_t / uint32_t / uint64_t / int / int64_t)
    ///   - std::string
    ///   - bool
    ///   - double
    ///   - std::vector<T> for the above scalars (TOML arrays)
    template <typename T>
    [[nodiscard]] std::expected<T, ConfigErrorInfo>
    get(std::string_view key) const {
        if (tbl_ == nullptr) {
            return std::unexpected(detail::make_missing(dotted(key)));
        }
        auto node = (*tbl_)[key];
        if (!node) {
            return std::unexpected(detail::make_missing(dotted(key)));
        }
        return extract<T>(node, dotted(key));
    }

    /// Optional-get with caller-supplied default. Missing key → returns `def`.
    /// Wrong-type is still an error (we fall back to `def` only for absence).
    template <typename T>
    [[nodiscard]] T get_or(std::string_view key, T def) const {
        if (!has(key)) return def;
        auto r = get<T>(key);
        return r ? std::move(*r) : def;
    }

private:
    std::string dotted(std::string_view key) const {
        std::string out = "scenarios.";
        out.append(name_).append(".").append(key);
        return out;
    }

    template <typename T>
    std::expected<T, ConfigErrorInfo>
    extract(toml::node_view<const toml::node> node,
            std::string dotted_key) const {
        if constexpr (std::is_same_v<T, std::string>) {
            if (auto v = node.value<std::string>(); v) return *v;
            return std::unexpected(detail::make_wrong_type(dotted_key, "string"));
        } else if constexpr (std::is_same_v<T, bool>) {
            if (auto v = node.value<bool>(); v) return *v;
            return std::unexpected(detail::make_wrong_type(dotted_key, "bool"));
        } else if constexpr (std::is_same_v<T, double>) {
            if (auto v = node.value<double>(); v) return *v;
            // TOML ints are silently promoted to doubles when requested.
            if (auto v = node.value<int64_t>(); v) return static_cast<double>(*v);
            return std::unexpected(detail::make_wrong_type(dotted_key, "double"));
        } else if constexpr (std::is_integral_v<T>) {
            if (auto v = node.value<int64_t>(); v) {
                // Range-check before narrowing. Without this, a typo'd
                // `port = 70000` silently truncates to 4464; `port = -1`
                // wraps to 65535. Either looks like a network failure
                // downstream and confuses operators. Surface the
                // out-of-range case at config-load time with the actual
                // value baked into the diagnostic.
                using Limits = std::numeric_limits<T>;
                if constexpr (std::is_unsigned_v<T>) {
                    if (*v < 0 ||
                        static_cast<uint64_t>(*v) >
                            static_cast<uint64_t>(Limits::max())) [[unlikely]] {
                        return std::unexpected(detail::make_wrong_type(
                            dotted_key,
                            "integer in range [0, " +
                                std::to_string(Limits::max()) + "] (got " +
                                std::to_string(*v) + ")"));
                    }
                } else {
                    if (*v < static_cast<int64_t>(Limits::min()) ||
                        *v > static_cast<int64_t>(Limits::max())) [[unlikely]] {
                        return std::unexpected(detail::make_wrong_type(
                            dotted_key,
                            "integer in range [" +
                                std::to_string(Limits::min()) + ", " +
                                std::to_string(Limits::max()) + "] (got " +
                                std::to_string(*v) + ")"));
                    }
                }
                return static_cast<T>(*v);
            }
            return std::unexpected(detail::make_wrong_type(dotted_key, "integer"));
        } else if constexpr (requires { typename T::value_type; }) {
            // std::vector<U>
            using U = typename T::value_type;
            const toml::array* arr = node.as_array();
            if (!arr) {
                return std::unexpected(detail::make_wrong_type(dotted_key, "array"));
            }
            T out;
            out.reserve(arr->size());
            for (std::size_t i = 0; i < arr->size(); ++i) {
                auto elem = arr->get(i);
                if constexpr (std::is_same_v<U, std::string>) {
                    if (auto v = elem->value<std::string>(); v) {
                        out.push_back(*v);
                        continue;
                    }
                } else if constexpr (std::is_integral_v<U>) {
                    if (auto v = elem->value<int64_t>(); v) {
                        // Mirror the scalar-path range check (see line ~283).
                        // Without this, a typo'd `ports = [70000]` silently
                        // truncates to vector{4464}, which then surfaces as
                        // an opaque connect-failure downstream. Surface the
                        // out-of-range case at config-load with the actual
                        // value baked into the diagnostic.
                        using Limits = std::numeric_limits<U>;
                        if constexpr (std::is_unsigned_v<U>) {
                            if (*v < 0 ||
                                static_cast<uint64_t>(*v) >
                                    static_cast<uint64_t>(Limits::max())) [[unlikely]] {
                                return std::unexpected(detail::make_wrong_type(
                                    dotted_key + "[" + std::to_string(i) + "]",
                                    "integer in range [0, " +
                                        std::to_string(Limits::max()) +
                                        "] (got " + std::to_string(*v) + ")"));
                            }
                        } else {
                            if (*v < static_cast<int64_t>(Limits::min()) ||
                                *v > static_cast<int64_t>(Limits::max())) [[unlikely]] {
                                return std::unexpected(detail::make_wrong_type(
                                    dotted_key + "[" + std::to_string(i) + "]",
                                    "integer in range [" +
                                        std::to_string(Limits::min()) + ", " +
                                        std::to_string(Limits::max()) +
                                        "] (got " + std::to_string(*v) + ")"));
                            }
                        }
                        out.push_back(static_cast<U>(*v));
                        continue;
                    }
                } else if constexpr (std::is_same_v<U, double>) {
                    if (auto v = elem->value<double>(); v) {
                        out.push_back(*v);
                        continue;
                    }
                    if (auto v = elem->value<int64_t>(); v) {
                        out.push_back(static_cast<double>(*v));
                        continue;
                    }
                }
                return std::unexpected(detail::make_wrong_type(
                    dotted_key + "[" + std::to_string(i) + "]",
                    "scalar"));
            }
            return out;
        } else {
            static_assert(!sizeof(T*),
                          "Scenario::get<T>: unsupported type");
        }
    }

    const toml::table* tbl_ = nullptr;
    std::string        name_;
};

// ─── BenchConfig: owner of the parsed toml::table plus structured fields ───

/// Structured view over a loaded bench `config.toml`.
///
/// Move-only: `scenarios` map entries hold non-owning pointers into `raw_`,
/// so copying would create aliasing lifetime issues. Move preserves the
/// pointers (toml::table move leaves sub-nodes at the same addresses).
class BenchConfig {
public:
    // Top-level sections.
    Networking     networking;
    Cpu            cpu;
    Measurement    measurement;
    DpdkRss        dpdk;
    ParallelConfig parallel;

    /// Lookup `[scenarios.<name>]`; returns nullptr if that scenario
    /// subtable is absent.
    [[nodiscard]] const Scenario* scenario(std::string_view name) const noexcept {
        auto it = scenarios_.find(std::string{name});
        return (it == scenarios_.end()) ? nullptr : &it->second;
    }

    /// Same as `scenario()` but aborts via std::terminate if the scenario
    /// is missing. Use only where the caller has already verified the
    /// scenario must exist (e.g. main() after argv parsing).
    [[nodiscard]] const Scenario& scenario_or_throw(std::string_view name) const {
        const Scenario* s = scenario(name);
        if (s == nullptr) {
            std::fprintf(stderr,
                         "bench::BenchConfig: scenario '%.*s' not found in config\n",
                         static_cast<int>(name.size()), name.data());
            std::terminate();
        }
        return *s;
    }

    /// All loaded scenario names, for introspection / logging.
    [[nodiscard]] std::vector<std::string> scenario_names() const {
        std::vector<std::string> out;
        out.reserve(scenarios_.size());
        for (const auto& [k, _] : scenarios_) out.push_back(k);
        return out;
    }

    /// The raw toml::table this config was parsed from. Exposed so edge
    /// cases that need a non-POD field can reach in; prefer the typed
    /// accessors above.
    [[nodiscard]] const toml::table& raw() const noexcept { return raw_; }

    /// The original path we loaded from (for diagnostic messages).
    [[nodiscard]] std::string_view source_path() const noexcept { return source_; }

    // Movable but not copyable — see class comment.
    BenchConfig() = default;
    BenchConfig(BenchConfig&&) = default;
    BenchConfig& operator=(BenchConfig&&) = default;
    BenchConfig(const BenchConfig&) = delete;
    BenchConfig& operator=(const BenchConfig&) = delete;

    // Private to the loader below — but we expose a named friend so the
    // free function can populate fields without exposing setters.
    friend std::expected<BenchConfig, ConfigErrorInfo>
    load_bench_conf(std::string_view path);

private:
    toml::table raw_;
    std::string source_;
    std::unordered_map<std::string, Scenario> scenarios_;
};

// ─── Loaders ───────────────────────────────────────────────────────────

/// Parse a TOML file into a `toml::table`. Generic helper for auxiliary
/// files (e.g. MMPP params) where the structured `BenchConfig` schema
/// doesn't apply.
[[nodiscard]] inline std::expected<toml::table, ConfigErrorInfo>
load_toml_table(std::string_view path) {
    if (path.empty()) {
        return std::unexpected(ConfigErrorInfo{
            ConfigError::FileNotFound, {}, "path is empty", {}, 0});
    }
    try {
        return toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        return std::unexpected(ConfigErrorInfo{
            ConfigError::ParseFailed,
            {},
            std::string{e.description()},
            std::string{path},
            static_cast<int>(e.source().begin.line)});
    } catch (const std::exception& e) {
        return std::unexpected(ConfigErrorInfo{
            ConfigError::FileNotFound,
            {},
            e.what(),
            std::string{path},
            0});
    }
}

/// Load and validate a bench `config.toml`. Performs required-field
/// checks on `networking.*` and `cpu.*`; scenarios are collected without
/// inner-field validation (each scenario binary validates its own
/// required fields at use time via Scenario::get<T>).
[[nodiscard]] inline std::expected<BenchConfig, ConfigErrorInfo>
load_bench_conf(std::string_view path) {
    auto tbl_r = load_toml_table(path);
    if (!tbl_r) return std::unexpected(std::move(tbl_r.error()));

    BenchConfig cfg;
    cfg.raw_    = std::move(*tbl_r);
    cfg.source_ = std::string{path};

    // ── networking ────────────────────────────────────────────────────
    auto& raw = cfg.raw_;
    if (auto* n = raw["networking"].as_table()) {
        auto get_s = [&](const char* key) -> std::optional<std::string> {
            if (auto v = (*n)[key].value<std::string>()) return v;
            return std::nullopt;
        };
        if (auto v = get_s("nic_a"))     cfg.networking.nic_a     = *v;
        if (auto v = get_s("nic_b"))     cfg.networking.nic_b     = *v;
        if (auto v = get_s("server_ip")) cfg.networking.server_ip = *v;
        if (auto v = get_s("client_ip")) cfg.networking.client_ip = *v;
        if (auto v = get_s("gateway_ip"))cfg.networking.gateway_ip= *v;
        if (auto v = get_s("netmask"))   cfg.networking.netmask   = *v;
        if (auto v = get_s("nic_b_pci")) cfg.networking.nic_b_pci = *v;
    }

    // Required networking keys.
    auto require_net = [&](std::string_view key, const std::string& val)
        -> std::expected<void, ConfigErrorInfo> {
        if (val.empty()) {
            return std::unexpected(detail::make_missing(
                std::string{"networking."} + std::string{key},
                cfg.source_));
        }
        return {};
    };
    if (auto r = require_net("nic_b",     cfg.networking.nic_b);     !r) return std::unexpected(std::move(r.error()));
    if (auto r = require_net("server_ip", cfg.networking.server_ip); !r) return std::unexpected(std::move(r.error()));
    if (auto r = require_net("client_ip", cfg.networking.client_ip); !r) return std::unexpected(std::move(r.error()));
    if (auto r = require_net("gateway_ip",cfg.networking.gateway_ip);!r) return std::unexpected(std::move(r.error()));

    // ── cpu ───────────────────────────────────────────────────────────
    if (auto* c = raw["cpu"].as_table()) {
        // Same int64→int narrowing concern as the measurement /
        // dpdk.rss / parallel.runs[] blocks below. CPU id range is
        // [0, INT_MAX] practically; reject obvious typos (negative
        // or above INT_MAX) up front so pin_thread() failure points
        // back at config rather than a misleading sched_setaffinity
        // EINVAL.
        if (auto v = (*c)["cpu_client"].value<int64_t>()) {
            if (*v < 0 || *v > std::numeric_limits<int>::max()) {
                return std::unexpected(ConfigErrorInfo{
                    .code   = ConfigError::InvalidValue,
                    .key    = "cpu.cpu_client",
                    .detail = std::to_string(*v) +
                              " out of range [0, INT_MAX]",
                    .file   = cfg.source_,
                    .line   = 0});
            }
            cfg.cpu.cpu_client = static_cast<int>(*v);
        }
        if (auto v = (*c)["cpu_mock"].value<int64_t>()) {
            if (*v < 0 || *v > std::numeric_limits<int>::max()) {
                return std::unexpected(ConfigErrorInfo{
                    .code   = ConfigError::InvalidValue,
                    .key    = "cpu.cpu_mock",
                    .detail = std::to_string(*v) +
                              " out of range [0, INT_MAX]",
                    .file   = cfg.source_,
                    .line   = 0});
            }
            cfg.cpu.cpu_mock = static_cast<int>(*v);
        }
        if (auto v = (*c)["eal_cores"].value<std::string>()) cfg.cpu.eal_cores = *v;
        if (auto v = (*c)["allow_non_isolated"].value<bool>()) cfg.cpu.allow_non_isolated = *v;
    }
    if (cfg.cpu.cpu_client < 0) {
        return std::unexpected(detail::make_missing("cpu.cpu_client", cfg.source_));
    }
    if (cfg.cpu.cpu_mock < 0) {
        return std::unexpected(detail::make_missing("cpu.cpu_mock", cfg.source_));
    }

    // ── measurement ──────────────────────────────────────────────────
    // toml++ surfaces integer values as int64_t; the destination fields
    // are narrower (uint32_t/uint16_t). Without a range-check a typo
    // like `warmup_samples = -1` or `nb_rx_queues = 100000` silently
    // truncates to 4294967295 / 34464 — both produce strange bench
    // behaviour without any breadcrumb pointing at config. Reject
    // out-of-range values up front so the operator sees the typo.
    if (auto* m = raw["measurement"].as_table()) {
        if (auto v = (*m)["warmup_samples"].value<int64_t>()) {
            if (*v < 0 || *v > std::numeric_limits<uint32_t>::max()) {
                return std::unexpected(ConfigErrorInfo{
                    .code   = ConfigError::InvalidValue,
                    .key    = "measurement.warmup_samples",
                    .detail = std::to_string(*v) +
                              " out of range [0, UINT32_MAX]",
                    .file   = cfg.source_,
                    .line   = 0});
            }
            cfg.measurement.warmup_samples = static_cast<uint32_t>(*v);
        }
        if (auto v = (*m)["server_work_ns"].value<int64_t>()) {
            if (*v < 0 || *v > std::numeric_limits<uint32_t>::max()) {
                return std::unexpected(ConfigErrorInfo{
                    .code   = ConfigError::InvalidValue,
                    .key    = "measurement.server_work_ns",
                    .detail = std::to_string(*v) +
                              " out of range [0, UINT32_MAX]",
                    .file   = cfg.source_,
                    .line   = 0});
            }
            cfg.measurement.server_work_ns = static_cast<uint32_t>(*v);
        }
    }

    // ── dpdk.rss ─────────────────────────────────────────────────────
    if (auto* d = raw["dpdk"].as_table()) {
        if (auto* rss = (*d)["rss"].as_table()) {
            if (auto v = (*rss)["nb_rx_queues"].value<int64_t>()) {
                // dpdk caps queues per port at uint16_t but practically
                // a NIC has ≤ 64 queues; >UINT16_MAX here is almost
                // certainly a typo. Reject negative + sub-1 too: 0 means
                // "no RX queues" which is undefined for the bench.
                if (*v < 1 || *v > std::numeric_limits<uint16_t>::max()) {
                    return std::unexpected(ConfigErrorInfo{
                        .code   = ConfigError::InvalidValue,
                        .key    = "dpdk.rss.nb_rx_queues",
                        .detail = std::to_string(*v) +
                                  " out of range [1, UINT16_MAX]",
                        .file   = cfg.source_,
                        .line   = 0});
                }
                cfg.dpdk.nb_rx_queues = static_cast<uint16_t>(*v);
            }
            if (auto v = (*rss)["lcore_per_queue"].value<std::string>())
                cfg.dpdk.lcore_per_queue = *v;
        }
    }

    // ── parallel ─────────────────────────────────────────────────────
    // Optional. Absent or empty `runs` → `lat all` has nothing to
    // dispatch (single-scenario commands continue to work).
    if (auto* p = raw["parallel"].as_table()) {
        if (auto* arr = (*p)["runs"].as_array()) {
            for (const auto& el : *arr) {
                const auto* tbl = el.as_table();
                if (tbl == nullptr) continue;
                RunSpec r;
                if (auto v = (*tbl)["scenario"].value<std::string>())
                    r.scenario = *v;
                // Same int64→uint16 narrowing concern as the
                // measurement / dpdk.rss block above: a typo like
                // `lcore = 100000` would silently wrap to 34464,
                // colliding with another lcore. Reject up front.
                if (auto v = (*tbl)["lcore"].value<int64_t>()) {
                    if (*v < 0 || *v > std::numeric_limits<uint16_t>::max()) {
                        return std::unexpected(ConfigErrorInfo{
                            .code   = ConfigError::InvalidValue,
                            .key    = "parallel.runs[].lcore",
                            .detail = std::to_string(*v) +
                                      " out of range [0, UINT16_MAX]",
                            .file   = cfg.source_,
                            .line   = 0});
                    }
                    r.lcore = static_cast<uint16_t>(*v);
                }
                if (auto v = (*tbl)["cpu"].value<int64_t>()) {
                    if (*v < 0 || *v > std::numeric_limits<int>::max()) {
                        return std::unexpected(ConfigErrorInfo{
                            .code   = ConfigError::InvalidValue,
                            .key    = "parallel.runs[].cpu",
                            .detail = std::to_string(*v) +
                                      " out of range [0, INT_MAX]",
                            .file   = cfg.source_,
                            .line   = 0});
                    }
                    r.cpu = static_cast<int>(*v);
                }
                if (auto v = (*tbl)["queue"].value<int64_t>()) {
                    if (*v < 0 || *v > std::numeric_limits<uint16_t>::max()) {
                        return std::unexpected(ConfigErrorInfo{
                            .code   = ConfigError::InvalidValue,
                            .key    = "parallel.runs[].queue",
                            .detail = std::to_string(*v) +
                                      " out of range [0, UINT16_MAX]",
                            .file   = cfg.source_,
                            .line   = 0});
                    }
                    r.queue = static_cast<uint16_t>(*v);
                }
                if (!r.scenario.empty()) {
                    cfg.parallel.runs.push_back(std::move(r));
                }
            }
        }
    }

    // ── scenarios.* ──────────────────────────────────────────────────
    if (auto* scenarios_tbl = raw["scenarios"].as_table()) {
        for (const auto& [k, v] : *scenarios_tbl) {
            if (!v.is_table()) continue;
            std::string name{k.str()};
            const toml::table* sub = v.as_table();
            cfg.scenarios_.emplace(name, Scenario(sub, name));
        }
    }

    return cfg;
}

} // namespace bench
