#pragma once

/// @file platform.hpp
/// Layer 1: DPDK Platform — header-only.
///
/// Full initialization sequence:
///   EAL init → port enumerate → mempool create →
///   port configure (NIC capability intersection) →
///   RX/TX queue setup → port start → link poll

#include "hal.hpp"
#include "hal_real.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <string>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

namespace eph_dpdk {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

/// Runtime configuration for the DPDK platform layer.
struct PlatformConfig {
    uint16_t port_id         = 0;
    uint16_t nb_rx_queues    = 1;
    uint16_t nb_tx_queues    = 1;
    uint16_t nb_rx_desc      = 256;   ///< Descriptors per RX queue
    uint16_t nb_tx_desc      = 512;   ///< Descriptors per TX queue
    /// Must be 2^n − 1 (e.g. 1023, 4095, 8191).
    /// DPDK silently rounds up otherwise — hard to debug.
    uint32_t mbuf_pool_size  = 4095;
    uint16_t mbuf_cache_size = 256;
    bool     enable_promiscuous = false;
    /// Poll timeout for link-up after port start (ms).
    /// 0 = single check, move on regardless.
    int      link_timeout_ms = 2000;
    LogLevel log_level       = LogLevel::Info;
};

/// Returns an error description if cfg is invalid, empty string if OK.
/// A non-empty string containing "2^n" indicates a performance warning
/// (not a hard error — DPDK will round up silently).
inline std::string validate_config(const PlatformConfig& cfg) {
    if (cfg.nb_rx_queues  == 0) return "nb_rx_queues must be > 0";
    if (cfg.nb_tx_queues  == 0) return "nb_tx_queues must be > 0";
    if (cfg.nb_rx_desc    == 0) return "nb_rx_desc must be > 0";
    if (cfg.nb_tx_desc    == 0) return "nb_tx_desc must be > 0";
    if (cfg.link_timeout_ms < 0) return "link_timeout_ms must be >= 0";

    uint32_t n = cfg.mbuf_pool_size + 1;
    if (n == 0 || (n & (n - 1)) != 0) {
        return std::format(
            "mbuf_pool_size {} is not (2^n - 1); DPDK will silently round up "
            "(next valid value: {})",
            cfg.mbuf_pool_size,
            (1u << (32 - std::countl_zero(n))) - 1u);
    }
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────

class DpdkPlatform {
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

    /// Create and fully initialize the DPDK platform.
    /// Pass a MockDpdkHal via @p hal to unit-test without real hardware;
    /// nullptr selects the real DPDK backend.
    static std::expected<DpdkPlatform, std::string>
    create(int argc, char** argv, const PlatformConfig& config,
           std::shared_ptr<IDpdkHal> hal = nullptr);

    ~DpdkPlatform();

    DpdkPlatform(const DpdkPlatform&)            = delete;
    DpdkPlatform& operator=(const DpdkPlatform&) = delete;
    DpdkPlatform(DpdkPlatform&&) noexcept;
    DpdkPlatform& operator=(DpdkPlatform&&) noexcept;

    rte_mempool* mempool()    const noexcept;
    uint16_t     port_id()    const noexcept;
    bool         is_running() const noexcept;

    Stats collect_stats() const;

private:
    // ── Impl (pimpl — fully defined below so unique_ptr destructor compiles)
    struct Impl;
    explicit DpdkPlatform(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline std::shared_ptr<spdlog::logger> platform_logger() {
    static auto l = [] {
        auto lg = spdlog::stdout_color_mt("dpdk.platform");
        lg->set_level(spdlog::level::trace);
        return lg;
    }();
    return l;
}

/// Clamp descriptor count into [lim.nb_min, lim.nb_max], aligned to lim.nb_align.
inline uint16_t clamp_desc(uint16_t requested, const rte_eth_desc_lim& lim) {
    uint16_t n = std::max(requested, lim.nb_min);
    n = std::min(n, lim.nb_max);
    if (lim.nb_align > 1)
        n = static_cast<uint16_t>(((n + lim.nb_align - 1) / lim.nb_align) * lim.nb_align);
    return n;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// DpdkPlatform::Impl definition
// ─────────────────────────────────────────────────────────────────────────────

struct DpdkPlatform::Impl {
    PlatformConfig            config;
    std::shared_ptr<IDpdkHal> hal;
    rte_mempool*              mempool{nullptr};
    bool                      eal_initialized{false};
    bool                      port_started{false};

    ~Impl() { cleanup(); }

    std::expected<void, std::string> init_eal(int argc, char** argv) {
        auto log = detail::platform_logger();
        SPDLOG_LOGGER_TRACE(log, "Calling rte_eal_init (argc={})", argc);

        int ret = hal->eal_init(argc, argv);
        if (ret < 0) {
            return std::unexpected(std::format(
                "rte_eal_init failed (ret={}, rte_errno={}): {}",
                ret, rte_errno, rte_strerror(rte_errno)));
        }
        eal_initialized = true;
        SPDLOG_LOGGER_DEBUG(log, "EAL initialized (ret={})", ret);
        return {};
    }

    std::expected<void, std::string> enumerate_ports() {
        auto log = detail::platform_logger();
        uint16_t count = hal->eth_dev_count_avail();

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
            config.mbuf_pool_size, config.mbuf_cache_size, RTE_MBUF_DEFAULT_BUF_SIZE);

        mempool = hal->pktmbuf_pool_create(
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
        SPDLOG_LOGGER_DEBUG(log, "mbuf pool created at {:p}", static_cast<void*>(mempool));
        return {};
    }

    std::expected<void, std::string> configure_port() {
        auto log = detail::platform_logger();

        // Query NIC capabilities first — offload flags MUST be intersected with
        // device capabilities; hard-coding flags is the most common portability bug.
        rte_eth_dev_info dev_info{};
        int ret = hal->eth_dev_info_get(config.port_id, dev_info);
        if (ret != 0) {
            SPDLOG_LOGGER_ERROR(log,
                "rte_eth_dev_info_get(port={}) failed: ret={}", config.port_id, ret);
            return std::unexpected(std::format(
                "eth_dev_info_get failed for port {} (ret={})", config.port_id, ret));
        }
        SPDLOG_LOGGER_DEBUG(log,
            "port={} driver={} max_rx_q={} max_tx_q={} "
            "rx_offload_capa={:#x} tx_offload_capa={:#x}",
            config.port_id,
            dev_info.driver_name ? dev_info.driver_name : "unknown",
            dev_info.max_rx_queues, dev_info.max_tx_queues,
            dev_info.rx_offload_capa, dev_info.tx_offload_capa);

        if (config.nb_rx_queues > dev_info.max_rx_queues)
            SPDLOG_LOGGER_WARN(log, "nb_rx_queues={} exceeds NIC max={}; clamping",
                config.nb_rx_queues, dev_info.max_rx_queues);
        if (config.nb_tx_queues > dev_info.max_tx_queues)
            SPDLOG_LOGGER_WARN(log, "nb_tx_queues={} exceeds NIC max={}; clamping",
                config.nb_tx_queues, dev_info.max_tx_queues);

        rte_eth_conf eth_conf{};
        std::memset(&eth_conf, 0, sizeof(eth_conf));
        // Intersection: requested offloads ∩ NIC capabilities.
        // Requesting unsupported flags causes configure() to fail with cryptic -EINVAL.
        eth_conf.rxmode.offloads = 0 & dev_info.rx_offload_capa;
        eth_conf.txmode.offloads = 0 & dev_info.tx_offload_capa;

        ret = hal->eth_dev_configure(config.port_id,
                                     config.nb_rx_queues, config.nb_tx_queues, eth_conf);
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
        hal->eth_dev_info_get(config.port_id, dev_info);

        uint16_t rx_desc = detail::clamp_desc(config.nb_rx_desc, dev_info.rx_desc_lim);
        uint16_t tx_desc = detail::clamp_desc(config.nb_tx_desc, dev_info.tx_desc_lim);

        if (rx_desc != config.nb_rx_desc)
            SPDLOG_LOGGER_WARN(log,
                "nb_rx_desc adjusted {} → {} [min={}, max={}, align={}]",
                config.nb_rx_desc, rx_desc,
                dev_info.rx_desc_lim.nb_min, dev_info.rx_desc_lim.nb_max,
                dev_info.rx_desc_lim.nb_align);
        if (tx_desc != config.nb_tx_desc)
            SPDLOG_LOGGER_WARN(log,
                "nb_tx_desc adjusted {} → {} [min={}, max={}, align={}]",
                config.nb_tx_desc, tx_desc,
                dev_info.tx_desc_lim.nb_min, dev_info.tx_desc_lim.nb_max,
                dev_info.tx_desc_lim.nb_align);

        SPDLOG_LOGGER_DEBUG(log,
            "Setting up {} RX queue(s) x {} descs, {} TX queue(s) x {} descs",
            config.nb_rx_queues, rx_desc, config.nb_tx_queues, tx_desc);

        for (uint16_t q = 0; q < config.nb_rx_queues; ++q) {
            int ret = hal->eth_rx_queue_setup(
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
            int ret = hal->eth_tx_queue_setup(
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
        SPDLOG_LOGGER_DEBUG(log, "All queues configured for port={}", config.port_id);
        return {};
    }

    std::expected<void, std::string> start_port() {
        auto log = detail::platform_logger();

        if (config.enable_promiscuous) {
            int ret = hal->eth_promiscuous_enable(config.port_id);
            if (ret != 0)
                SPDLOG_LOGGER_WARN(log,
                    "eth_promiscuous_enable(port={}) failed: ret={} (non-fatal)",
                    config.port_id, ret);
        }

        int ret = hal->eth_dev_start(config.port_id);
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

        auto check_once = [&]() -> bool {
            rte_eth_link link{};
            hal->eth_link_get_nowait(config.port_id, link);
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

        int64_t deadline = hal->now_ms() + config.link_timeout_ms;
        while (hal->now_ms() < deadline) {
            if (check_once()) return;
            hal->sleep_ms(10);
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
            hal->eth_dev_stop(config.port_id);
            hal->eth_dev_close(config.port_id);
            port_started = false;
        }
        if (mempool != nullptr) {
            SPDLOG_LOGGER_DEBUG(log, "Freeing mbuf pool {:p}", static_cast<void*>(mempool));
            hal->mempool_free(mempool);
            mempool = nullptr;
        }
        if (eal_initialized) {
            SPDLOG_LOGGER_DEBUG(log, "Calling rte_eal_cleanup");
            hal->eal_cleanup();
            eal_initialized = false;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// DpdkPlatform method definitions
// ─────────────────────────────────────────────────────────────────────────────

inline DpdkPlatform::DpdkPlatform(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

inline DpdkPlatform::~DpdkPlatform() = default;

inline DpdkPlatform::DpdkPlatform(DpdkPlatform&&) noexcept            = default;
inline DpdkPlatform& DpdkPlatform::operator=(DpdkPlatform&&) noexcept = default;

inline std::expected<DpdkPlatform, std::string>
DpdkPlatform::create(int argc, char** argv, const PlatformConfig& config,
                     std::shared_ptr<IDpdkHal> hal) {
    auto log = detail::platform_logger();

    // 0. Validate config
    if (auto err = validate_config(config); !err.empty()) {
        if (err.find("2^n") != std::string::npos) {
            SPDLOG_LOGGER_WARN(log, "PlatformConfig warning: {}", err);
        } else {
            SPDLOG_LOGGER_ERROR(log, "Invalid PlatformConfig: {}", err);
            return std::unexpected(err);
        }
    }

    if (hal == nullptr) hal = make_real_hal();

    auto impl    = std::make_unique<Impl>();
    impl->config = config;
    impl->hal    = std::move(hal);

    // Steps 1–7: each returns early on failure; Impl destructor cleans up.
    if (auto r = impl->init_eal(argc, argv);   !r) return std::unexpected(r.error());
    if (auto r = impl->enumerate_ports();       !r) return std::unexpected(r.error());
    if (auto r = impl->create_mempool();        !r) return std::unexpected(r.error());
    if (auto r = impl->configure_port();        !r) return std::unexpected(r.error());
    if (auto r = impl->setup_queues();          !r) return std::unexpected(r.error());
    if (auto r = impl->start_port();            !r) return std::unexpected(r.error());
    impl->wait_link_up();

    SPDLOG_LOGGER_INFO(log, "DpdkPlatform ready (port={})", config.port_id);
    return DpdkPlatform(std::move(impl));
}

inline rte_mempool* DpdkPlatform::mempool()    const noexcept { return impl_->mempool; }
inline uint16_t     DpdkPlatform::port_id()    const noexcept { return impl_->config.port_id; }
inline bool         DpdkPlatform::is_running() const noexcept { return impl_->port_started; }

inline DpdkPlatform::Stats DpdkPlatform::collect_stats() const {
    auto log = detail::platform_logger();
    rte_eth_stats raw{};
    int ret = impl_->hal->eth_stats_get(impl_->config.port_id, raw);
    if (ret != 0) {
        SPDLOG_LOGGER_WARN(log,
            "eth_stats_get(port={}) failed: ret={}", impl_->config.port_id, ret);
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

} // namespace eph_dpdk
