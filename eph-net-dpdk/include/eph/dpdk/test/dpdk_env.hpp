#pragma once

/// @file test/dpdk_env.hpp
/// Shared DPDK bootstrap helper for tests and benchmarks.
///
/// `DpdkBenchEnv::create(pcfg, eal_cfg, pins, pin_policy, mock_ip,
/// client_ip, gateway_ip)` performs:
///   1. Platform::create_with_eal — EAL init (typed-pin or raw) +
///      Platform construction in a single call. The Platform owns
///      the EAL session and runs eal_cleanup automatically on
///      destruction.
///   2. Parse local/server/gateway IPs to host byte order
///   3. Get local MAC via rte_eth_macaddr_get()
///   4. ARP-resolve gateway MAC
///
/// The result is a move-only struct containing all the resources needed
/// to create UdpSender, TcpSession, or WS transports over DPDK.
///
/// Originally lived under benchmarks/latency/core/dpdk_env.hpp; promoted
/// to eph-dpdk's test namespace so any test fixture or benchmark can
/// reuse this bring-up sequence without reverse-including the bench
/// tree.  Type name retained as `DpdkBenchEnv` for source compatibility.

#ifdef EPH_USE_DPDK

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <unistd.h>     // ::unlink / ::getpid for atomic gw_mac publish
#include <cstdio>       // ::rename for atomic gw_mac publish
#include <rte_ethdev.h>

#include <spdlog/spdlog.h>

#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/lcore_pin.hpp"
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/platform.hpp"
// JoinDynamicConfig comes in transitively via platform.hpp (post-api-unify
// reshape — sentinel guard EPH_DPDK_PLATFORM_CONFIG_DEFINED requires
// platform.hpp first).
#include "eph/dpdk/tcp.hpp"
#include "eph/dpdk/udp.hpp"
#include "eph/utils/cpu.hpp"

namespace eph::dpdk::test {

/// @brief Parse an EAL-style lcore CSV ("0,1,2", "0-3", "0-1,4-5",
/// or empty) into a 64-bit `lcore_mask` suitable for MpRegistry's
/// v2 cross-process conflict scan.
///
/// The 64-bit cap is a hard schema limit: `ProcSlot::lcore_mask` is
/// `uint64_t`, so lcore IDs >= 64 cannot be represented. This parser
/// rejects any input that names an lcore outside `[0, 63]` with
/// `std::unexpected` rather than silently dropping the bit — the
/// silent-drop variant lets misuse slip past the conflict gate (two
/// peers both pinning lcore 70 would get mask=0 each, no overlap,
/// no rejection — exactly the silent-failure mode the v2 schema
/// was added to close).
///
/// Input forms accepted:
///   - empty string         → mask=0 (back-compat opt-out for callers
///                            that haven't migrated to v2 tracking)
///   - "0,1,2"              → mask=0x7
///   - "0-3"                → mask=0xF
///   - "0-1,4-5"            → mask=0x33
///   - " 0 , 1 "            → mask=0x3 (whitespace tolerated)
///
/// Input forms rejected with std::unexpected (descriptive message):
///   - "abc"                → malformed integer
///   - "5-3"                → reversed range
///   - "-3"                 → negative integer (strtoul wraps to a
///                            huge unsigned, caught by >=64 check)
///   - "64"                 → lcore ID exceeds schema cap
///   - "60-65"              → range upper bound exceeds schema cap
///
/// Discovered via /pax --review LENS "production / MP mental model
/// / silent misuse closure" round 3 on 2026-05-01: the previous
/// inlined parser used `for (i = lo; i <= hi && i < 64; ++i)` which
/// silently truncated any bit at or above index 64, defeating the
/// v2 conflict gate on >64-core hosts. The hard-error policy here
/// is the symmetric closure of that gap.
[[nodiscard]] inline std::expected<uint64_t, std::string>
parse_lcores_csv_to_mask(std::string_view lcores) noexcept {
    constexpr unsigned long kMaxLcoreId = 63;
    uint64_t mask = 0;
    // Use a heap-free copy when the buffer is short — for the
    // EAL-CSV case this is always tens of bytes max. We scan in
    // place over `lcores.data()` with a manual index since strtoul
    // wants a NUL-terminated buffer; copy into a fixed-size local.
    // Bench callers pass strings up to ~64 chars; cap at 256.
    if (lcores.size() > 256) {
        return std::unexpected(
            "parse_lcores_csv_to_mask: input too long (> 256 bytes)");
    }
    char buf[260];
    std::memcpy(buf, lcores.data(), lcores.size());
    buf[lcores.size()] = '\0';
    const char* p = buf;
    while (*p) {
        while (*p == ' ' || *p == ',') ++p;
        if (!*p) break;
        char* end = nullptr;
        // Reject a leading '-' explicitly so strtoul doesn't wrap a
        // negative input into a huge unsigned that then evades the
        // >=64 check below by satisfying `i < 64` being false (loop
        // skipped, mask=0, no error). The end-pointer check that
        // follows would NOT catch this because strtoul DID consume
        // the digits; only the wrap-around damage is silent.
        if (*p == '-' || *p == '+') {
            return std::unexpected(
                std::string{"parse_lcores_csv_to_mask: signed integer "
                            "rejected at '"} + p + "' (lcore IDs must "
                            "be non-negative)");
        }
        unsigned long lo = std::strtoul(p, &end, 10);
        if (end == p) {
            return std::unexpected(
                std::string{"parse_lcores_csv_to_mask: malformed "
                            "integer near '"} + p + "'");
        }
        if (lo > kMaxLcoreId) {
            return std::unexpected(
                std::string{"parse_lcores_csv_to_mask: lcore "} +
                std::to_string(lo) +
                " exceeds MpRegistry schema cap of " +
                std::to_string(kMaxLcoreId) +
                " (ProcSlot::lcore_mask is uint64_t — IDs >= 64 "
                "cannot be tracked)");
        }
        unsigned long hi = lo;
        p = end;
        if (*p == '-') {
            ++p;
            // Same signed-integer guard for the upper bound.
            if (*p == '-' || *p == '+') {
                return std::unexpected(
                    std::string{"parse_lcores_csv_to_mask: signed "
                                "integer rejected at '"} + p + "' "
                                "(range upper bound must be "
                                "non-negative)");
            }
            hi = std::strtoul(p, &end, 10);
            if (end == p) {
                return std::unexpected(
                    std::string{"parse_lcores_csv_to_mask: malformed "
                                "range upper bound near '"} + p + "'");
            }
            if (hi < lo) {
                return std::unexpected(
                    std::string{"parse_lcores_csv_to_mask: reversed "
                                "range "} + std::to_string(lo) + "-" +
                    std::to_string(hi));
            }
            if (hi > kMaxLcoreId) {
                return std::unexpected(
                    std::string{"parse_lcores_csv_to_mask: range upper "
                                "bound "} + std::to_string(hi) +
                    " exceeds MpRegistry schema cap of " +
                    std::to_string(kMaxLcoreId));
            }
            p = end;
        }
        for (unsigned long i = lo; i <= hi; ++i) {
            mask |= (uint64_t{1} << i);
        }
    }
    return mask;
}

/// Move-only bundle of all DPDK resources needed to drive a real-NIC
/// scenario: Platform (which owns EAL — see `Platform::create_with_eal`),
/// resolved src/dst/gw IPs (host byte order), local MAC, gateway MAC
/// (ARP-resolved at startup), and the port/pool handles used by
/// sender factory methods.
///
/// Construct via `create()`. The resulting object owns its Platform,
/// which in turn owns the EAL session — the destructor chain
/// (~DpdkBenchEnv → ~Platform → eal_cleanup) handles every release in
/// the correct order, no manual eal_cleanup needed.
struct DpdkBenchEnv {
    eph::dpdk::Platform  platform;
    uint32_t             src_ip{};      ///< local IP (host byte order)
    uint32_t             dst_ip{};      ///< server IP (host byte order)
    uint32_t             gw_ip{};       ///< gateway IP (host byte order)
    rte_ether_addr       src_mac{};
    rte_ether_addr       gw_mac{};
    uint16_t             port_id{};
    rte_mempool*         pool{nullptr};

    DpdkBenchEnv(eph::dpdk::Platform p,
                 uint32_t si, uint32_t di, uint32_t gi,
                 rte_ether_addr sm, rte_ether_addr gm,
                 uint16_t pid, rte_mempool* pl)
        : platform(std::move(p)),
          src_ip(si), dst_ip(di), gw_ip(gi),
          src_mac(sm), gw_mac(gm), port_id(pid), pool(pl) {}

    DpdkBenchEnv(DpdkBenchEnv&&) = default;
    DpdkBenchEnv& operator=(DpdkBenchEnv&&) = default;

    /// One-shot factory: brings up Platform via `create_with_eal`,
    /// then ARP-resolves the gateway and packages everything into a
    /// move-only struct. The returned env owns Platform and Platform
    /// owns EAL — destruction is fully RAII.
    ///
    /// @param pcfg        Full PlatformConfig (port_id, nb_rx_queues,
    ///                    enable_promiscuous, mbuf_pool_size, etc.).
    /// @param eal_cfg     EAL configuration. Caller-owned strings.
    /// @param pins        Typed lcore→cpu pin spec. Empty span = no
    ///                    typed-pin path. Mutually exclusive with
    ///                    `eal_cfg.lcores`.
    /// @param pin_policy  Strictness for the typed-pin validator.
    /// @param mock_ip     Server IP (mock peer) — dotted-quad string.
    /// @param client_ip   Local source IP — dotted-quad string.
    /// @param gateway_ip  Default gateway IP for ARP resolve — dotted-quad.
    [[nodiscard]] static std::expected<DpdkBenchEnv, std::string>
    create(eph::dpdk::PlatformConfig pcfg,
           eph::dpdk::EalConfig eal_cfg,
           std::span<eph::dpdk::LcorePin const> pins,
           eph::utils::CpuPinPolicy pin_policy,
           const std::string& mock_ip,
           const std::string& client_ip,
           const std::string& gateway_ip) {
        // ── 1. EAL + Platform via the unified factory ──────────────
        auto plat = eph::dpdk::Platform::create_with_eal(
            std::move(pcfg), std::move(eal_cfg), pins, pin_policy);
        if (!plat) return std::unexpected("DpdkBenchEnv::create: " + plat.error());

        const uint16_t port_id = plat->port_id();
        rte_mempool* const pool = plat->mempool();

        // ── 2. Parse IPs (host byte order) ─────────────────────────
        auto parse_ip = [](const std::string& s) -> std::expected<uint32_t, std::string> {
            in_addr addr{};
            if (inet_pton(AF_INET, s.c_str(), &addr) != 1) {
                return std::unexpected("invalid IP: " + s);
            }
            return ntohl(addr.s_addr);
        };
        auto src_ip = parse_ip(client_ip);
        if (!src_ip) return std::unexpected(src_ip.error());
        auto dst_ip = parse_ip(mock_ip);
        if (!dst_ip) return std::unexpected(dst_ip.error());
        auto gw_ip = parse_ip(gateway_ip);
        if (!gw_ip) return std::unexpected(gw_ip.error());

        // ── 3. Local MAC ───────────────────────────────────────────
        rte_ether_addr src_mac{};
        if (int rc = rte_eth_macaddr_get(port_id, &src_mac); rc != 0) {
            return std::unexpected("rte_eth_macaddr_get failed: " +
                                   std::to_string(rc));
        }
        SPDLOG_INFO("dpdk_env: local MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                    src_mac.addr_bytes[0], src_mac.addr_bytes[1],
                    src_mac.addr_bytes[2], src_mac.addr_bytes[3],
                    src_mac.addr_bytes[4], src_mac.addr_bytes[5]);

        // ── 4. ARP resolve gateway MAC ─────────────────────────────
        auto gw_mac_result = eph::dpdk::arp::resolve(
            port_id, pool,
            src_mac, *src_ip, *gw_ip,
            std::chrono::seconds{3});
        if (!gw_mac_result) {
            return std::unexpected("ARP resolve gateway: " + gw_mac_result.error());
        }
        SPDLOG_INFO("dpdk_env: gateway MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                    gw_mac_result->addr_bytes[0], gw_mac_result->addr_bytes[1],
                    gw_mac_result->addr_bytes[2], gw_mac_result->addr_bytes[3],
                    gw_mac_result->addr_bytes[4], gw_mac_result->addr_bytes[5]);

        return DpdkBenchEnv{
            std::move(*plat),
            *src_ip, *dst_ip, *gw_ip,
            src_mac, *gw_mac_result,
            port_id, pool
        };
    }

    /// Autojoin variant of `create()`: brings up Platform via
    /// `Platform::join_dynamic` instead of `create_with_eal`.
    /// Use this when running multiple bench-client processes on the
    /// same NIC simultaneously (e.g. each `lat_<sc>_dpdk` binary as
    /// its own MP process, sharing the NIC via RSS-partitioned
    /// queues). The first peer auto-resolves to primary, subsequent
    /// peers to secondaries; each owns a disjoint `[qlo, qhi)` RX
    /// queue range and src_port window.
    ///
    /// Single-process callers should keep using `create()` — its
    /// path is byte-equivalent to legacy and unaffected by this
    /// addition.
    ///
    /// **TX queue gotcha** (reshape/rss-aware-connect retro #1):
    /// `pcfg_template.nb_tx_queues` is set to `max_procs` here so a
    /// secondary peer's RSS-aware `tx_queue_id` (= its owned
    /// `target_qid`, possibly > 0) maps to a real TX ring. Default
    /// `nb_tx_queues = 1` would silently leave secondary's SYN
    /// stranded.
    ///
    /// **ARP under MP**: secondary peers cannot ARP-resolve the
    /// gateway directly because ARP replies hash to whichever queue
    /// the NIC's default RSS picks (typically queue 0, owned by
    /// primary). This factory therefore resolves ARP only on the
    /// primary peer; secondaries publish/read the gateway MAC via
    /// `gw_mac_share_path`. Caller is responsible for picking a
    /// path the orchestrator coordinates (e.g. derived from BDF).
    ///
    /// @param pci_bdf            NIC PCI BDF, e.g. "0000:28:00.0"
    /// @param max_procs          Autojoin slot count; used as both
    ///                            `JoinDynamicConfig::max_procs` and
    ///                            `pcfg_template.{nb_rx_queues,
    ///                            nb_tx_queues}`
    /// @param lcores             EAL lcore CSV for this peer (e.g.
    ///                            `"0,1"`). Auto-parsed into a bitmask
    ///                            for the registry's cross-process
    ///                            lcore-conflict check; supports
    ///                            individual ids (`"0,2,4"`) and
    ///                            ranges (`"0-3"` or `"0-1,4-5"`).
    ///                            Empty string → no lcore tracking
    ///                            (back-compat opt-out).
    /// @param mock_ip            Server IP — dotted-quad
    /// @param client_ip          Local source IP — dotted-quad
    /// @param gateway_ip         Gateway IP for ARP — dotted-quad
    /// @param gw_mac_share_path  File path for cross-peer gw_mac
    ///                            sharing. Primary writes; secondary
    ///                            polls until it appears (timeout 3s).
    [[nodiscard]] static std::expected<DpdkBenchEnv, std::string>
    create_via_autojoin(const std::string& pci_bdf,
                        uint32_t max_procs,
                        const std::string& lcores,
                        const std::string& mock_ip,
                        const std::string& client_ip,
                        const std::string& gateway_ip,
                        const std::string& gw_mac_share_path) {
        if (max_procs < 2) {
            return std::unexpected(
                "DpdkBenchEnv::create_via_autojoin: max_procs must be >= 2 "
                "(use create() for single-process)");
        }

        // Empty gw_mac_share_path is a silent-failure mode: primary's
        // std::ofstream("") fails with a useless "" diagnostic, secondary
        // polls "" for 3 s and times out the same way. Reject up front
        // and name the env var so an orchestrator misconfig is
        // self-diagnosing.
        //
        // Discovered via /pax --review LENS "production / MP mental model
        // / silent misuse closure" round 6 on 2026-05-01.
        if (gw_mac_share_path.empty()) {
            SPDLOG_ERROR(
                "DpdkBenchEnv::create_via_autojoin: gw_mac_share_path is "
                "empty — orchestrator must set EPH_LAT_AUTOJOIN_GW_MAC_FILE "
                "(or pass a non-empty path) so primary peer can publish the "
                "ARP-resolved gateway MAC for secondaries to consume");
            return std::unexpected(
                "DpdkBenchEnv::create_via_autojoin: gw_mac_share_path is "
                "empty (set EPH_LAT_AUTOJOIN_GW_MAC_FILE to a writable path "
                "shared between primary and secondary peers)");
        }

        // Reject an empty lcores string up-front. parse_lcores_csv_to_mask
        // accepts "" → mask=0 as a deliberate back-compat for legacy callers
        // that haven't migrated to v2 tracking, but the autojoin path
        // ALWAYS wants v2 conflict detection: a zero mask means "I claim
        // no lcores", which silently disables the cross-peer overlap
        // gate. This pairs with the gw_mac_share_path empty-guard above —
        // both are MP-mental-model silent-misuse closures.
        if (lcores.empty()) {
            SPDLOG_ERROR(
                "DpdkBenchEnv::create_via_autojoin: lcores is empty — "
                "autojoin requires a non-empty CSV (e.g. \"0,1\" or "
                "\"0-3\") so MpRegistry v2 can detect cross-peer lcore "
                "overlap; orchestrator must set EPH_LAT_AUTOJOIN_LCORES "
                "or pass a non-empty value");
            return std::unexpected(
                "DpdkBenchEnv::create_via_autojoin: lcores is empty "
                "(set EPH_LAT_AUTOJOIN_LCORES to a non-empty CSV so "
                "MpRegistry v2 conflict detection has a non-zero mask)");
        }

        // Parse lcore CSV into bitmask for cross-process conflict
        // detection (MpRegistry v2). Delegated to the standalone
        // helper above so the parser is unit-testable without EAL,
        // and so the schema-cap guard (lcore IDs >= 64 hard-error
        // rather than silent drop) lives in one place.
        auto mask_r = parse_lcores_csv_to_mask(lcores);
        if (!mask_r) {
            return std::unexpected(
                "DpdkBenchEnv::create_via_autojoin: " + mask_r.error());
        }
        const uint64_t self_lcore_mask = *mask_r;
        // Defensive: the empty-guard above should already preclude
        // mask==0, but `parse_lcores_csv_to_mask("  ,  ")` (whitespace
        // and commas only) also evaluates to 0 — close that path here
        // rather than let it propagate as a silent no-claim.
        if (self_lcore_mask == 0) {
            SPDLOG_ERROR(
                "DpdkBenchEnv::create_via_autojoin: lcores='{}' parsed "
                "to mask=0 — MpRegistry v2 cannot detect overlap with "
                "an empty claim; CSV must contain at least one lcore",
                lcores);
            return std::unexpected(
                "DpdkBenchEnv::create_via_autojoin: lcores='" + lcores
                + "' parsed to mask=0 (CSV resolved to no lcore IDs)");
        }

        // ── 1. Platform::join_dynamic ──────────────────────────────
        eph::dpdk::JoinDynamicConfig jd{};
        jd.pci                          = pci_bdf;
        jd.pcfg_template.nb_rx_queues   = static_cast<uint16_t>(max_procs);
        jd.pcfg_template.nb_tx_queues   = static_cast<uint16_t>(max_procs);
        jd.max_procs                    = static_cast<uint16_t>(max_procs);
        jd.lcores                       = {lcores};
        jd.self_lcore_mask              = self_lcore_mask;

        auto plat = eph::dpdk::Platform::join_dynamic(std::move(jd));
        if (!plat) {
            return std::unexpected(
                "DpdkBenchEnv::create_via_autojoin: " + plat.error());
        }

        const uint16_t port_id   = plat->port_id();
        rte_mempool* const pool  = plat->mempool();
        const bool is_secondary  = plat->is_secondary();

        // ── 2. Parse IPs (host byte order) ─────────────────────────
        auto parse_ip = [](const std::string& s)
            -> std::expected<uint32_t, std::string> {
            in_addr addr{};
            if (inet_pton(AF_INET, s.c_str(), &addr) != 1) {
                return std::unexpected("invalid IP: " + s);
            }
            return ntohl(addr.s_addr);
        };
        auto src_ip = parse_ip(client_ip);
        if (!src_ip) return std::unexpected(src_ip.error());
        auto dst_ip = parse_ip(mock_ip);
        if (!dst_ip) return std::unexpected(dst_ip.error());
        auto gw_ip = parse_ip(gateway_ip);
        if (!gw_ip) return std::unexpected(gw_ip.error());

        // ── 3. Local MAC ───────────────────────────────────────────
        rte_ether_addr src_mac{};
        if (int rc = rte_eth_macaddr_get(port_id, &src_mac); rc != 0) {
            return std::unexpected(
                "rte_eth_macaddr_get failed: " + std::to_string(rc));
        }
        SPDLOG_INFO("dpdk_env: [{}] local MAC "
                    "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                    is_secondary ? "secondary" : "primary",
                    src_mac.addr_bytes[0], src_mac.addr_bytes[1],
                    src_mac.addr_bytes[2], src_mac.addr_bytes[3],
                    src_mac.addr_bytes[4], src_mac.addr_bytes[5]);

        // ── 4. ARP (primary) or shared-file read (secondary) ───────
        //
        // Cross-peer gw_mac handoff via shared file. The publish/consume
        // protocol must tolerate three failure modes that previously
        // silent-failed (round 6 audit, 2026-05-01):
        //
        //   (1) STALE FILE: a previous bench session left a gw_mac file at
        //       the same path (orchestrator typically derives the path
        //       from the BDF, which is stable across runs). Without unlink
        //       on the primary side, the secondary's poll-and-read loop
        //       breaks out on the *first* iteration with the *previous*
        //       run's MAC bytes — long before the current primary's ARP
        //       resolves. Symptom: secondary uses stale GW MAC, kernel
        //       mock side blackholes frames as wrong-dst-MAC, bench
        //       reports "no echo replies" with no GW pointer.
        //
        //   (2) PARTIAL READ: ofstream's per-character writes are not
        //       atomic; a secondary that opens the file mid-write can
        //       read e.g. only 4 bytes of a 6-byte MAC and either fail
        //       sscanf or (worse) match a 6-token line that's been
        //       half-overwritten. Mitigation: write-tmp + rename(2) so
        //       the secondary either sees the old file (unlinked above
        //       in the same call) or the fully-populated new file —
        //       never a partial.
        //
        //   (3) ALL-ZERO MAC: 00:00:00:00:00:00 is never a valid gateway
        //       and indicates either a bug (file written before ARP
        //       completed) or attacker-injected content. Reject on the
        //       secondary's parse path.
        rte_ether_addr gw_mac{};
        if (!is_secondary) {
            // (1) Unlink any stale file from a previous run BEFORE ARP
            // resolves. ENOENT (no previous file) is the expected
            // first-run case; any other errno is a real I/O failure
            // we surface so the user sees the path we couldn't clean.
            if (::unlink(gw_mac_share_path.c_str()) != 0 && errno != ENOENT) {
                const int saved_errno = errno;
                SPDLOG_ERROR(
                    "DpdkBenchEnv::create_via_autojoin: failed to unlink "
                    "stale gw_mac file '{}' (errno={} {})",
                    gw_mac_share_path, saved_errno, std::strerror(saved_errno));
                return std::unexpected(
                    "primary failed to unlink stale gw_mac at " +
                    gw_mac_share_path + " (errno=" +
                    std::to_string(saved_errno) + " " +
                    std::strerror(saved_errno) + ")");
            }
            SPDLOG_INFO(
                "DpdkBenchEnv::create_via_autojoin: cleared any stale "
                "gw_mac file at '{}' before ARP resolve",
                gw_mac_share_path);

            auto gw_r = eph::dpdk::arp::resolve(
                port_id, pool, src_mac, *src_ip, *gw_ip,
                std::chrono::seconds{3});
            if (!gw_r) {
                return std::unexpected(
                    "ARP resolve gateway: " + gw_r.error());
            }
            gw_mac = *gw_r;
            // (2) Atomic publish: write to <path>.tmp.<pid>, then
            // ::rename to the final path. POSIX guarantees rename(2) is
            // atomic on the same filesystem, so any concurrent
            // secondary reader sees either no file (still polling) or
            // the fully-published one — never a partial write.
            const std::string tmp_path =
                gw_mac_share_path + ".tmp." + std::to_string(::getpid());
            {
                std::ofstream f(tmp_path);
                if (!f.is_open()) {
                    SPDLOG_ERROR(
                        "DpdkBenchEnv::create_via_autojoin: cannot open "
                        "tmp publish file '{}' for write", tmp_path);
                    return std::unexpected(
                        "cannot write gw_mac tmp file: " + tmp_path);
                }
                char buf[32];
                std::snprintf(buf, sizeof(buf),
                              "%02x:%02x:%02x:%02x:%02x:%02x\n",
                              gw_mac.addr_bytes[0], gw_mac.addr_bytes[1],
                              gw_mac.addr_bytes[2], gw_mac.addr_bytes[3],
                              gw_mac.addr_bytes[4], gw_mac.addr_bytes[5]);
                f << buf;
                if (!f.good()) {
                    SPDLOG_ERROR(
                        "DpdkBenchEnv::create_via_autojoin: write to tmp "
                        "publish file '{}' failed (stream bad)", tmp_path);
                    (void)::unlink(tmp_path.c_str());
                    return std::unexpected(
                        "gw_mac tmp file write failed: " + tmp_path);
                }
                // f closes on scope exit — content flushed before rename.
            }
            if (::rename(tmp_path.c_str(), gw_mac_share_path.c_str()) != 0) {
                const int saved_errno = errno;
                SPDLOG_ERROR(
                    "DpdkBenchEnv::create_via_autojoin: rename '{}' → "
                    "'{}' failed (errno={} {})",
                    tmp_path, gw_mac_share_path,
                    saved_errno, std::strerror(saved_errno));
                (void)::unlink(tmp_path.c_str());
                return std::unexpected(
                    "gw_mac atomic rename failed: " + tmp_path + " → " +
                    gw_mac_share_path + " (errno=" +
                    std::to_string(saved_errno) + " " +
                    std::strerror(saved_errno) + ")");
            }
            SPDLOG_DEBUG(
                "DpdkBenchEnv::create_via_autojoin: atomically published "
                "gw_mac to '{}' via tmp '{}'",
                gw_mac_share_path, tmp_path);
        } else {
            // Poll up to 3s for the primary to publish.
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds{3};
            std::string line;
            while (std::chrono::steady_clock::now() < deadline) {
                std::ifstream f(gw_mac_share_path);
                if (f.is_open() && std::getline(f, line) && !line.empty()) {
                    break;
                }
                line.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (line.empty()) {
                return std::unexpected(
                    "secondary timed out waiting for gw_mac at " +
                    gw_mac_share_path);
            }
            unsigned int b[6];
            if (std::sscanf(line.c_str(), "%x:%x:%x:%x:%x:%x",
                            &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
                return std::unexpected(
                    "secondary failed to parse gw_mac line: '" + line + "'");
            }
            for (int i = 0; i < 6; ++i) {
                gw_mac.addr_bytes[i] = static_cast<uint8_t>(b[i]);
            }
            // (3) All-zero MAC is never a valid gateway. Reject so a
            // primary that wrote before ARP completed (or attacker
            // injection / file truncation) doesn't silently corrupt
            // the secondary's TX path.
            const bool all_zero = (gw_mac.addr_bytes[0] | gw_mac.addr_bytes[1] |
                                   gw_mac.addr_bytes[2] | gw_mac.addr_bytes[3] |
                                   gw_mac.addr_bytes[4] | gw_mac.addr_bytes[5]) == 0;
            if (all_zero) {
                SPDLOG_ERROR(
                    "DpdkBenchEnv::create_via_autojoin: secondary read "
                    "all-zero gw_mac from '{}' — primary wrote before ARP "
                    "completed, file truncated, or attacker injection",
                    gw_mac_share_path);
                return std::unexpected(
                    "secondary read all-zero gw_mac at " + gw_mac_share_path +
                    " (00:00:00:00:00:00 is never a valid gateway — primary "
                    "wrote before ARP completed or file is corrupt)");
            }
        }
        SPDLOG_INFO("dpdk_env: [{}] gateway MAC "
                    "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                    is_secondary ? "secondary" : "primary",
                    gw_mac.addr_bytes[0], gw_mac.addr_bytes[1],
                    gw_mac.addr_bytes[2], gw_mac.addr_bytes[3],
                    gw_mac.addr_bytes[4], gw_mac.addr_bytes[5]);

        return DpdkBenchEnv{
            std::move(*plat),
            *src_ip, *dst_ip, *gw_ip,
            src_mac, gw_mac,
            port_id, pool
        };
    }

    /// Create a UdpSender configured for this environment.
    [[nodiscard]] std::expected<eph::dpdk::UdpSender, std::string>
    make_udp_sender(uint16_t local_port, uint16_t remote_port) const {
        eph::dpdk::UdpConfig ucfg{};
        ucfg.src_ip = src_ip;
        ucfg.dst_ip = dst_ip;
        ucfg.src_port = local_port;
        ucfg.dst_port = remote_port;
        ucfg.src_mac = src_mac;
        ucfg.dst_mac = gw_mac;
        ucfg.port_id = port_id;
        ucfg.tx_queue_id = 0;
        ucfg.pool = pool;
        return eph::dpdk::UdpSender::create(ucfg);
    }

    /// Create a TcpConfig for TcpSession.
    [[nodiscard]] eph::dpdk::TcpConfig
    make_tcp_config(uint16_t local_port, uint16_t remote_port) const {
        eph::dpdk::TcpConfig tcfg{};
        tcfg.tuple = {src_ip, dst_ip, local_port, remote_port};
        tcfg.src_mac = src_mac;
        tcfg.dst_mac = gw_mac;
        tcfg.port_id = port_id;
        tcfg.tx_queue_id = 0;
        tcfg.rx_queue_id = 0;
        return tcfg;
    }
};

} // namespace eph::dpdk::test

#endif // EPH_USE_DPDK
