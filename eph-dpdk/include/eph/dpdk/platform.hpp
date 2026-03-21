#pragma once

/// @file platform.hpp
/// Layer 1: DPDK Platform — header-only.
///
/// Assumes EAL is already initialized (see eal.hpp).
///
/// Initialization sequence:
///   port enumerate → mempool create →
///   port configure (NIC capability intersection) →
///   RX/TX queue setup → port start → link poll
///
/// Compile-time philosophy:
///   - PlatformConfig fields that are structurally constrained (pool size
///     must be 2^n-1, queues/descs must be > 0) are validated at compile
///     time when the config is constexpr, and at runtime otherwise.
///   - clamp_desc is constexpr — usable in static assertions if NIC
///     descriptor limits are known ahead of time.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

namespace eph::dpdk {

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time utilities
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Check if n is of the form (2^k - 1) for some k >= 1.
/// Rejects 0 (which is technically 2^0-1 but not a valid pool size).
constexpr bool is_power_of_two_minus_one(uint32_t n) noexcept {
    if (n == 0) return false;
    uint32_t m = n + 1;
    return m > 0 && (m & (m - 1)) == 0;
}

/// Next valid pool size >= n that satisfies the 2^k-1 constraint.
constexpr uint32_t next_valid_pool_size(uint32_t n) noexcept {
    uint32_t m = n + 1;
    // Round up to next power of 2.
    if (m == 0 || (m & (m - 1)) == 0) return m - 1;
    return (1u << (32 - std::countl_zero(m))) - 1u;
}

/// Clamp descriptor count into [lim.nb_min, lim.nb_max],
/// aligned to lim.nb_align.
///
/// constexpr — can be used in static_assert when NIC descriptor limits
/// are known at compile time (e.g. from a constexpr dev_info fixture).
constexpr uint16_t clamp_desc(uint16_t requested,
                               uint16_t nb_min,
                               uint16_t nb_max,
                               uint16_t nb_align) noexcept {
    uint16_t n = std::max(requested, nb_min);
    n = std::min(n, nb_max);
    if (nb_align > 1)
        n = static_cast<uint16_t>(
            ((n + nb_align - 1) / nb_align) * nb_align);
    return n;
}

/// Overload accepting rte_eth_desc_lim (not constexpr — struct from DPDK).
inline uint16_t clamp_desc(uint16_t requested,
                            const rte_eth_desc_lim& lim) noexcept {
    return clamp_desc(requested, lim.nb_min, lim.nb_max, lim.nb_align);
}

inline std::shared_ptr<spdlog::logger> platform_logger() {
    static auto l = [] {
        auto lg = spdlog::stdout_color_mt("dpdk.platform");
        // Inherit level from spdlog global default
        return lg;
    }();
    return l;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

struct PlatformConfig {
    uint16_t port_id         = 0;
    uint16_t nb_rx_queues    = 1;
    uint16_t nb_tx_queues    = 1;
    uint16_t nb_rx_desc      = 256;
    uint16_t nb_tx_desc      = 512;
    /// Must be 2^n − 1 (e.g. 1023, 4095, 8191).
    uint32_t mbuf_pool_size  = 4095;
    uint16_t mbuf_cache_size = 256;
    bool     enable_promiscuous = false;
    /// Poll timeout for link-up after port start (ms).
    /// 0 = single check, move on regardless.
    int      link_timeout_ms = 2000;
};

/// Validation result — empty string_view on success, error description otherwise.
/// constexpr-evaluable: use in static_assert for compile-time configs, or
/// call at runtime for dynamic configs.
///
/// A result containing "2^n" is a performance warning (DPDK rounds up silently),
/// not a hard error.
constexpr std::string_view validate_config(const PlatformConfig& cfg) noexcept {
    if (cfg.nb_rx_queues  == 0) return "nb_rx_queues must be > 0";
    if (cfg.nb_tx_queues  == 0) return "nb_tx_queues must be > 0";
    if (cfg.nb_rx_desc    == 0) return "nb_rx_desc must be > 0";
    if (cfg.nb_tx_desc    == 0) return "nb_tx_desc must be > 0";
    if (cfg.link_timeout_ms < 0) return "link_timeout_ms must be >= 0";
    if (!detail::is_power_of_two_minus_one(cfg.mbuf_pool_size))
        return "mbuf_pool_size must be 2^n - 1 (e.g. 1023, 4095, 8191)";
    return {};
}

/// For use in static_assert with constexpr configs:
///   constexpr PlatformConfig cfg{...};
///   static_assert(config_ok(cfg), "bad platform config");
constexpr bool config_ok(const PlatformConfig& cfg) noexcept {
    return validate_config(cfg).empty();
}

// ─────────────────────────────────────────────────────────────────────────────
// Platform
// ─────────────────────────────────────────────────────────────────────────────

class Platform {
public:
    struct Stats {
        uint64_t rx_packets = 0;
        uint64_t tx_packets = 0;
        uint64_t rx_bytes   = 0;
        uint64_t tx_bytes   = 0;
        uint64_t rx_missed  = 0;
        uint64_t rx_errors  = 0;
        uint64_t tx_errors  = 0;
    };

    /// Create and fully initialize the DPDK platform for one port.
    /// EAL must already be initialized (via eph::dpdk::eal_init).
    static std::expected<Platform, std::string>
    create(const PlatformConfig& config);

    ~Platform();

    Platform(const Platform&)            = delete;
    Platform& operator=(const Platform&) = delete;
    Platform(Platform&&) noexcept;
    Platform& operator=(Platform&&) noexcept;

    [[nodiscard]] rte_mempool* mempool()    const noexcept;
    [[nodiscard]] uint16_t     port_id()    const noexcept;
    [[nodiscard]] bool         is_running() const noexcept;

    [[nodiscard]] Stats collect_stats() const;

private:
    struct Impl;
    explicit Platform(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Platform::Impl
// ─────────────────────────────────────────────────────────────────────────────

struct Platform::Impl {
    PlatformConfig config;
    rte_mempool*   mempool{nullptr};
    bool           port_started{false};

    ~Impl() { cleanup(); }

    std::expected<void, std::string> enumerate_ports() {
        auto log = detail::platform_logger();
        uint16_t count = rte_eth_dev_count_avail();

        if (count == 0) {
            SPDLOG_LOGGER_ERROR(log,
                "No DPDK ports available (count=0); "
                "check VFIO binding and hugepage configuration");
            return std::unexpected("No DPDK ports available; check VFIO binding");
        }
        SPDLOG_LOGGER_INFO(log, "Available DPDK ports: {}", count);

        if (config.port_id >= count) {
            SPDLOG_LOGGER_ERROR(log,
                "Requested port_id={} but only {} port(s) available",
                config.port_id, count);
            return std::unexpected(std::format(
                "port_id {} out of range (available ports: {})",
                config.port_id, count));
        }
        return {};
    }

    std::expected<void, std::string> create_mempool() {
        auto log = detail::platform_logger();
        SPDLOG_LOGGER_DEBUG(log,
            "Creating mbuf pool: size={}, cache={}, data_room={}",
            config.mbuf_pool_size, config.mbuf_cache_size,
            RTE_MBUF_DEFAULT_BUF_SIZE);

        mempool = rte_pktmbuf_pool_create(
            "mbuf_pool", config.mbuf_pool_size, config.mbuf_cache_size,
            0, RTE_MBUF_DEFAULT_BUF_SIZE, SOCKET_ID_ANY);

        if (mempool == nullptr) {
            SPDLOG_LOGGER_ERROR(log,
                "rte_pktmbuf_pool_create failed: pool_size={}, rte_errno={}: {}",
                config.mbuf_pool_size, rte_errno, rte_strerror(rte_errno));
            return std::unexpected(std::format(
                "Failed to create mbuf pool (rte_errno={}): {}",
                rte_errno, rte_strerror(rte_errno)));
        }
        SPDLOG_LOGGER_DEBUG(log, "mbuf pool created at {:p}",
                            static_cast<void*>(mempool));
        return {};
    }

    std::expected<void, std::string> configure_port() {
        auto log = detail::platform_logger();

        // Query NIC capabilities — offload flags MUST be intersected with
        // device caps; hard-coding flags is the most common portability bug.
        rte_eth_dev_info dev_info{};
        int ret = rte_eth_dev_info_get(config.port_id, &dev_info);
        if (ret != 0) {
            SPDLOG_LOGGER_ERROR(log,
                "rte_eth_dev_info_get(port={}) failed: ret={}",
                config.port_id, ret);
            return std::unexpected(std::format(
                "eth_dev_info_get failed for port {} (ret={})",
                config.port_id, ret));
        }
        SPDLOG_LOGGER_DEBUG(log,
            "port={} driver={} max_rx_q={} max_tx_q={} "
            "rx_offload_capa={:#x} tx_offload_capa={:#x}",
            config.port_id,
            dev_info.driver_name ? dev_info.driver_name : "unknown",
            dev_info.max_rx_queues, dev_info.max_tx_queues,
            dev_info.rx_offload_capa, dev_info.tx_offload_capa);

        if (config.nb_rx_queues > dev_info.max_rx_queues)
            SPDLOG_LOGGER_WARN(log,
                "nb_rx_queues={} exceeds NIC max={}; clamping",
                config.nb_rx_queues, dev_info.max_rx_queues);
        if (config.nb_tx_queues > dev_info.max_tx_queues)
            SPDLOG_LOGGER_WARN(log,
                "nb_tx_queues={} exceeds NIC max={}; clamping",
                config.nb_tx_queues, dev_info.max_tx_queues);

        rte_eth_conf eth_conf{};
        std::memset(&eth_conf, 0, sizeof(eth_conf));
        // Intersection: requested offloads ∩ NIC capabilities.
        eth_conf.rxmode.offloads = 0 & dev_info.rx_offload_capa;
        eth_conf.txmode.offloads = 0 & dev_info.tx_offload_capa;

        ret = rte_eth_dev_configure(config.port_id,
                                    config.nb_rx_queues,
                                    config.nb_tx_queues,
                                    &eth_conf);
        if (ret != 0) {
            SPDLOG_LOGGER_ERROR(log,
                "rte_eth_dev_configure(port={}) failed: ret={}: {}",
                config.port_id, ret, rte_strerror(-ret));
            return std::unexpected(std::format(
                "eth_dev_configure failed for port {} (ret={}): {}",
                config.port_id, ret, rte_strerror(-ret)));
        }
        SPDLOG_LOGGER_DEBUG(log, "port={} configured", config.port_id);
        return {};
    }

    std::expected<void, std::string> setup_queues() {
        auto log = detail::platform_logger();

        rte_eth_dev_info dev_info{};
        if (int ret = rte_eth_dev_info_get(config.port_id, &dev_info); ret != 0) {
            SPDLOG_LOGGER_ERROR(log,
                "rte_eth_dev_info_get(port={}) failed in setup_queues: ret={}",
                config.port_id, ret);
            return std::unexpected(std::format(
                "eth_dev_info_get failed for port {} (ret={})",
                config.port_id, ret));
        }

        uint16_t rx_desc = detail::clamp_desc(config.nb_rx_desc,
                                               dev_info.rx_desc_lim);
        uint16_t tx_desc = detail::clamp_desc(config.nb_tx_desc,
                                               dev_info.tx_desc_lim);

        if (rx_desc != config.nb_rx_desc)
            SPDLOG_LOGGER_WARN(log,
                "nb_rx_desc adjusted {} -> {} [min={}, max={}, align={}]",
                config.nb_rx_desc, rx_desc,
                dev_info.rx_desc_lim.nb_min, dev_info.rx_desc_lim.nb_max,
                dev_info.rx_desc_lim.nb_align);
        if (tx_desc != config.nb_tx_desc)
            SPDLOG_LOGGER_WARN(log,
                "nb_tx_desc adjusted {} -> {} [min={}, max={}, align={}]",
                config.nb_tx_desc, tx_desc,
                dev_info.tx_desc_lim.nb_min, dev_info.tx_desc_lim.nb_max,
                dev_info.tx_desc_lim.nb_align);

        SPDLOG_LOGGER_DEBUG(log,
            "Setting up {} RX queue(s) x {} descs, {} TX queue(s) x {} descs",
            config.nb_rx_queues, rx_desc, config.nb_tx_queues, tx_desc);

        for (uint16_t q = 0; q < config.nb_rx_queues; ++q) {
            int ret = rte_eth_rx_queue_setup(
                config.port_id, q, rx_desc, SOCKET_ID_ANY, nullptr, mempool);
            if (ret != 0) {
                SPDLOG_LOGGER_ERROR(log,
                    "eth_rx_queue_setup(port={}, queue={}) failed: ret={}: {}",
                    config.port_id, q, ret, rte_strerror(-ret));
                return std::unexpected(std::format(
                    "eth_rx_queue_setup failed (port={}, queue={}, ret={}): {}",
                    config.port_id, q, ret, rte_strerror(-ret)));
            }
        }
        for (uint16_t q = 0; q < config.nb_tx_queues; ++q) {
            int ret = rte_eth_tx_queue_setup(
                config.port_id, q, tx_desc, SOCKET_ID_ANY, nullptr);
            if (ret != 0) {
                SPDLOG_LOGGER_ERROR(log,
                    "eth_tx_queue_setup(port={}, queue={}) failed: ret={}: {}",
                    config.port_id, q, ret, rte_strerror(-ret));
                return std::unexpected(std::format(
                    "eth_tx_queue_setup failed (port={}, queue={}, ret={}): {}",
                    config.port_id, q, ret, rte_strerror(-ret)));
            }
        }
        SPDLOG_LOGGER_DEBUG(log, "All queues configured for port={}",
                            config.port_id);
        return {};
    }

    std::expected<void, std::string> start_port() {
        auto log = detail::platform_logger();

        if (config.enable_promiscuous) {
            int ret = rte_eth_promiscuous_enable(config.port_id);
            if (ret != 0)
                SPDLOG_LOGGER_WARN(log,
                    "eth_promiscuous_enable(port={}) failed: ret={} (non-fatal)",
                    config.port_id, ret);
        }

        int ret = rte_eth_dev_start(config.port_id);
        if (ret != 0) {
            SPDLOG_LOGGER_ERROR(log,
                "rte_eth_dev_start(port={}) failed: ret={}: {}",
                config.port_id, ret, rte_strerror(-ret));
            return std::unexpected(std::format(
                "eth_dev_start failed for port {} (ret={}): {}",
                config.port_id, ret, rte_strerror(-ret)));
        }
        port_started = true;
        SPDLOG_LOGGER_DEBUG(log, "port={} started", config.port_id);
        return {};
    }

    void wait_link_up() {
        auto log = detail::platform_logger();
        using namespace std::chrono;

        auto check_once = [&]() -> bool {
            rte_eth_link link{};
            // Return value only indicates query failure, not link state.
            // On failure, link struct is zeroed → link_status == DOWN.
            [[maybe_unused]] int ret = rte_eth_link_get_nowait(config.port_id, &link);
            if (link.link_status == RTE_ETH_LINK_UP) {
                SPDLOG_LOGGER_INFO(log, "port={} link up: {} Mbps {}",
                    config.port_id, link.link_speed,
                    link.link_duplex ? "full-duplex" : "half-duplex");
                return true;
            }
            return false;
        };

        if (config.link_timeout_ms == 0) {
            if (!check_once())
                SPDLOG_LOGGER_WARN(log,
                    "port={} link not yet up (timeout=0); "
                    "link may negotiate asynchronously", config.port_id);
            return;
        }

        auto deadline = steady_clock::now()
                      + milliseconds(config.link_timeout_ms);
        while (steady_clock::now() < deadline) {
            if (check_once()) return;
            std::this_thread::sleep_for(milliseconds(10));
        }
        SPDLOG_LOGGER_WARN(log,
            "port={} link not yet up after {}ms; "
            "continuing — link may negotiate asynchronously",
            config.port_id, config.link_timeout_ms);
    }

    void cleanup() noexcept {
        auto log = detail::platform_logger();
        if (port_started) {
            SPDLOG_LOGGER_DEBUG(log, "Stopping port={}", config.port_id);
            rte_eth_dev_stop(config.port_id);
            rte_eth_dev_close(config.port_id);
            port_started = false;
        }
        if (mempool != nullptr) {
            SPDLOG_LOGGER_DEBUG(log, "Freeing mbuf pool {:p}",
                                static_cast<void*>(mempool));
            rte_mempool_free(mempool);
            mempool = nullptr;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Platform method definitions
// ─────────────────────────────────────────────────────────────────────────────

inline Platform::Platform(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

inline Platform::~Platform() = default;

inline Platform::Platform(Platform&&) noexcept            = default;
inline Platform& Platform::operator=(Platform&&) noexcept = default;

inline std::expected<Platform, std::string>
Platform::create(const PlatformConfig& config) {
    auto log = detail::platform_logger();

    if (auto err = validate_config(config); !err.empty()) {
        SPDLOG_LOGGER_ERROR(log, "Invalid PlatformConfig: {}", err);
        return std::unexpected(std::string{err});
    }

    auto impl    = std::make_unique<Impl>();
    impl->config = config;

    if (auto r = impl->enumerate_ports();  !r) return std::unexpected(r.error());
    if (auto r = impl->create_mempool();   !r) return std::unexpected(r.error());
    if (auto r = impl->configure_port();   !r) return std::unexpected(r.error());
    if (auto r = impl->setup_queues();     !r) return std::unexpected(r.error());
    if (auto r = impl->start_port();       !r) return std::unexpected(r.error());
    impl->wait_link_up();

    SPDLOG_LOGGER_INFO(log, "Platform ready (port={})", config.port_id);
    return Platform(std::move(impl));
}

inline rte_mempool* Platform::mempool()    const noexcept { return impl_->mempool; }
inline uint16_t     Platform::port_id()    const noexcept { return impl_->config.port_id; }
inline bool         Platform::is_running() const noexcept { return impl_->port_started; }

inline Platform::Stats Platform::collect_stats() const {
    auto log = detail::platform_logger();
    rte_eth_stats raw{};
    int ret = rte_eth_stats_get(impl_->config.port_id, &raw);
    if (ret != 0) {
        SPDLOG_LOGGER_WARN(log,
            "eth_stats_get(port={}) failed: ret={}",
            impl_->config.port_id, ret);
        return {};
    }
    return Stats{
        .rx_packets = raw.ipackets,
        .tx_packets = raw.opackets,
        .rx_bytes   = raw.ibytes,
        .tx_bytes   = raw.obytes,
        .rx_missed  = raw.imissed,
        .rx_errors  = raw.ierrors,
        .tx_errors  = raw.oerrors,
    };
}

} // namespace eph::dpdk
