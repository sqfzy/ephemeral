#pragma once

/// @file multi_port_platform.hpp
/// Layer 1.5: Multi-port DPDK platform aggregator — header-only.
///
/// Strictly additive over `eph::dpdk::Platform`. Owns N independent
/// `Platform` instances (one per physical NIC port) and exposes them
/// by index. The aggregator does NOT change any single-port semantics
/// — `Platform::create_primary` / `create_secondary` remain the
/// canonical entry points for the dominant 1:1 deployment.
///
/// Why a thin wrapper, not a generalized `Platform` with N port_ids:
///   - `Platform` owns one mempool / one port_id / one ICMP registry /
///     one per-queue Poller registry today. Spreading those across N
///     ports inside one struct would change every accessor's contract
///     (`port_id()`, `mempool()`, `register_poller()`, …) and force
///     every existing call site to learn a port-index parameter.
///   - The aggregation use cases (separate MD vs OE NIC, AWS multi-ENI,
///     redundant feeds) are coarse-grained: callers pick "the MD
///     platform" or "the OE platform" once at startup and pass that
///     reference into per-stream attach. They do not need a single
///     fused object.
///   - This wrapper preserves the option to compose by reference
///     instead of by index, and never gets in the way of `Platform`'s
///     own evolution.
///
/// Multi-port concerns intentionally NOT solved here:
///   - Cross-port ICMP routing. Each per-port `Platform` already owns
///     its own `IcmpRegistry`. A router-originated ICMP message
///     arrives on the same port (and same NIC MAC) the offending
///     packet was sent from, so dispatching it to the per-port
///     registry is correct. The aggregator deliberately does NOT
///     auto-merge registries: a stream attached to port A must not
///     receive ICMP messages aimed at a flow on port B (different
///     5-tuple, different connection) — sharing would be a bug, not
///     a feature.
///   - Multi-port `Poller`. `DpdkPoller` is per-queue (1:1 with one
///     `Platform`'s queue). Users that want to drive multiple ports
///     instantiate multiple Pollers; the aggregator does not
///     introduce a fused multi-port poller. Out of scope.
///   - Source-port partitioning. Same caller-responsibility contract
///     as the multi-process case (see `docs/dpdk-multiprocess.md`).
///     The aggregator has no global view across ports either.

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/core/error.hpp"          // core::Error / core::ErrorInfo
#include "eph/dpdk/detail/logger.hpp"  // get_logger / LoggerName
#include "eph/dpdk/platform.hpp"

namespace eph::dpdk {

namespace detail {

/// Dedicated logger for the multi-port aggregator.
inline spdlog::logger* multi_port_logger() {
    return get_logger<LoggerName{"dpdk.multi_port"}>();
}

}  // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// MultiPortPlatform
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Owns N independent `Platform` instances, one per NIC port.
///
/// Construction validates the input span (non-empty, distinct port_ids,
/// each `PlatformConfig` passes `validate_config`) and then delegates
/// each port's bringup to `Platform::create_primary` (the dominant
/// deployment role; secondary attach has its own factory and is
/// orthogonal to "I have N physical NICs in one process").
///
/// Per-port access is by index `[0, num_ports())`. The index is the
/// position in the input span — it is NOT necessarily equal to the DPDK
/// `port_id`. Use `port(i).port_id()` to get the DPDK port id.
///
/// Move-only. After move, accessors on the moved-from instance are
/// unsafe (the underlying vector is empty); guard via `num_ports() > 0`
/// or simply do not touch a moved-from aggregator.
///
/// Lifetime: each owned `Platform` is fully constructed when `create()`
/// returns; destruction frees them in reverse order (vector default).
/// If construction fails partway through (port K of N), every
/// already-constructed Platform is destroyed cleanly via RAII before
/// the error propagates back to the caller — no leaked NIC state.
class MultiPortPlatform {
public:
    /// @brief Build an aggregator over a span of per-port configs.
    ///
    /// Validation, in order:
    ///   1. `configs.empty()` → InvalidConfig ("at least one port required").
    ///   2. For each `cfg`, `validate_config(cfg)` must pass; the first
    ///      failure stops construction and returns InvalidConfig.
    ///   3. All `cfg.port_id` values must be distinct; the first
    ///      duplicate stops construction.
    ///   4. Each port is brought up via `Platform::create_primary`.
    ///      A failure at step K propagates immediately; ports `[0, K)`
    ///      are destroyed by the local vector's destructor.
    ///
    /// On success, returns a `unique_ptr<MultiPortPlatform>` with
    /// `num_ports() == configs.size()`. The pointer is movable and the
    /// aggregator owns every `Platform` until destruction.
    [[nodiscard]] static std::expected<std::unique_ptr<MultiPortPlatform>,
                                        ::eph::core::ErrorInfo>
    create(std::span<const PlatformConfig> configs) noexcept {
        [[maybe_unused]] auto log = detail::multi_port_logger();

        if (configs.empty()) {
            SPDLOG_LOGGER_ERROR(log,
                "MultiPortPlatform::create called with empty configs span");
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "MultiPortPlatform::create: configs span is empty "
                "(at least one PlatformConfig is required)"});
        }

        // Pre-pass 1: validate every config structurally before
        // touching any DPDK state. Cheap and surfaces the user error
        // at the call site rather than after spending I/O on port 0.
        for (std::size_t i = 0; i < configs.size(); ++i) {
            auto err = validate_config(configs[i]);
            if (!err.empty()) {
                SPDLOG_LOGGER_ERROR(log,
                    "MultiPortPlatform::create: configs[{}] invalid: {}",
                    i, err);
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::InvalidConfig,
                    "MultiPortPlatform::create: a PlatformConfig "
                    "failed validate_config (see WARN log for index "
                    "and reason)"});
            }
        }

        // Pre-pass 2: distinct port_id check. O(N^2) is fine — N is
        // the number of physical NICs in one box (typically 2-4, never
        // more than the host's PCIe slot count).
        for (std::size_t i = 0; i < configs.size(); ++i) {
            for (std::size_t j = i + 1; j < configs.size(); ++j) {
                if (configs[i].port_id == configs[j].port_id) {
                    SPDLOG_LOGGER_ERROR(log,
                        "MultiPortPlatform::create: duplicate port_id={} "
                        "at configs[{}] and configs[{}] — each port must "
                        "appear at most once in the aggregator",
                        configs[i].port_id, i, j);
                    return std::unexpected(::eph::core::ErrorInfo{
                        ::eph::core::Error::InvalidConfig,
                        "MultiPortPlatform::create: duplicate port_id "
                        "in configs (each NIC port may appear at most "
                        "once in a single aggregator)"});
                }
            }
        }

        // Real work: bring up each Platform. If a later port fails,
        // the local vector's destructor reaps every already-built
        // Platform via RAII before we surface the error.
        std::vector<Platform> ports;
        ports.reserve(configs.size());
        for (std::size_t i = 0; i < configs.size(); ++i) {
            auto r = Platform::create_primary(configs[i]);
            if (!r) {
                SPDLOG_LOGGER_ERROR(log,
                    "MultiPortPlatform::create: Platform::create_primary "
                    "failed for configs[{}] (port_id={}): {} — rolling "
                    "back any already-constructed ports",
                    i, configs[i].port_id, r.error());
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::InvalidConfig,
                    "MultiPortPlatform::create: Platform::create_primary "
                    "failed for one of the configs (see ERROR log for "
                    "index and underlying message)"});
            }
            ports.emplace_back(std::move(*r));
        }

        // `make_unique` would require a public ctor; we use `new`
        // explicitly because the wrapper ctor is private (callers
        // must go through `create()`).
        SPDLOG_LOGGER_INFO(log,
            "MultiPortPlatform ready: num_ports={}", ports.size());
        return std::unique_ptr<MultiPortPlatform>(
            new MultiPortPlatform(std::move(ports)));
    }

    ~MultiPortPlatform() = default;

    MultiPortPlatform(const MultiPortPlatform&)            = delete;
    MultiPortPlatform& operator=(const MultiPortPlatform&) = delete;
    MultiPortPlatform(MultiPortPlatform&&) noexcept            = default;
    MultiPortPlatform& operator=(MultiPortPlatform&&) noexcept = default;

    /// @brief Number of ports owned by this aggregator.
    [[nodiscard]] std::size_t num_ports() const noexcept {
        return ports_.size();
    }

    /// @brief Mutable access to the K-th `Platform`.
    /// @pre `idx < num_ports()` — out-of-range is a programming error;
    ///      the contract mirrors `std::vector::operator[]`. Use
    ///      `num_ports()` to gate.
    [[nodiscard]] Platform& port(std::size_t idx) noexcept {
        return ports_[idx];
    }

    /// @brief Read-only access to the K-th `Platform`. Same precondition
    ///        as the non-const overload.
    [[nodiscard]] const Platform& port(std::size_t idx) const noexcept {
        return ports_[idx];
    }

    /// @brief Linear scan helper: find the index of the first owned
    ///        Platform whose `port_id() == dpdk_port_id`. Returns
    ///        `std::size_t(-1)` (i.e. `npos`) when no port matches.
    ///
    /// O(N) over `num_ports()`. Cold-path only — call once at attach
    /// time to translate a DPDK port_id (e.g. coming from a
    /// configuration file) into an aggregator slot. Hot-path code
    /// should keep the slot index it received from `create()`'s input
    /// order.
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    [[nodiscard]] std::size_t
    find_index_by_port_id(uint16_t dpdk_port_id) const noexcept {
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            if (ports_[i].port_id() == dpdk_port_id) return i;
        }
        return npos;
    }

private:
    /// Construction is private; callers must go through `create()`.
    explicit MultiPortPlatform(std::vector<Platform> ports) noexcept
        : ports_(std::move(ports)) {}

    std::vector<Platform> ports_;
};

}  // namespace eph::dpdk
