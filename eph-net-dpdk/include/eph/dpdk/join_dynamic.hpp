#pragma once

/// @file join_dynamic.hpp
/// `JoinDynamicConfig` — the user-facing config for the **autojoin**
/// MP factory (`Platform::join_dynamic`). Autojoin is the
/// zero-coordination multi-process bring-up: two unrelated processes
/// that share a NIC agree on nothing except the PCI BDF and
/// `pcfg_template.nb_rx_queues`. Whoever `rte_eal_init`s first
/// becomes primary; later peers attach as secondaries and CAS-claim
/// the next free process slot themselves.
///
/// **Mental model**: `JoinDynamicConfig` only carries
/// **autojoin-specific** inputs — everything `PlatformConfig` already
/// has (per_lcore_pools / mbuf_pool_size / port_id / nb_rx_queues /
/// every other field) flows through `pcfg_template`. Autojoin only
/// overrides three fields it must own: `proc_type` (resolved at EAL
/// init), `mp_topology` (synthesized from claimed slot), and
/// `file_prefix` (auto-derived from BDF unless caller overrides).
/// Single source of truth: PlatformConfig.
///
/// Compared to the **declarative** path (`Platform::create_primary` /
/// `create_secondary` + a hand-written `MpTopology`), autojoin
/// trades per-peer slot precision for *no protocol* between peers:
///   - file_prefix is auto-derived from the BDF (so two peers naming
///     the same NIC name the same hugepage segment without sharing
///     a string)
///   - max_procs is auto-derived from `pcfg_template.nb_rx_queues /
///     queues_per_proc`
///   - self_index is auto-claimed at attach time (CAS-strong against
///     the registry's `procs[].claimed` flags)
///
/// The declarative path is preserved for the cases that need it:
/// deliberate asymmetric assignment, tagged process roles, or callers
/// that already wrote out their topology.
///
/// Hot path: NONE. `Platform::join_dynamic` is the cold setup factory;
/// once the Platform is up, the runtime path is byte-for-byte
/// identical to a declarative-path Platform with the same self_index.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "eph/dpdk/lcore_pin.hpp"   // LcorePin (typed pin spec)
#include "eph/utils/cpu.hpp"        // CpuPinPolicy

// This header references `eph::dpdk::PlatformConfig` (`pcfg_template`
// is a value member) but cannot directly `#include
// "eph/dpdk/platform.hpp"` without a circular include — platform.hpp
// includes this header from inside its own body, after PlatformConfig
// is fully parsed. The contract: include `eph/dpdk/platform.hpp` to
// get `JoinDynamicConfig`. Users who try to include this header
// standalone get a loud diagnostic instead of a confusing
// "PlatformConfig has not been declared".
#ifndef EPH_DPDK_PLATFORM_CONFIG_DEFINED
#  error "Include eph/dpdk/platform.hpp instead of join_dynamic.hpp directly: " \
         "JoinDynamicConfig embeds a PlatformConfig and must be parsed " \
         "after PlatformConfig is fully defined."
#endif

namespace eph::dpdk {

/// @brief Autojoin (`Platform::join_dynamic`) configuration.
///
/// Required: `pci`, `pcfg_template.nb_rx_queues > 0`. Everything
/// else has a sensible default.
struct JoinDynamicConfig {
    // ─── Autojoin-specific inputs ────────────────────────────────────────

    /// PCI BDF of the NIC to attach (e.g. `"0000:28:00.0"` or short
    /// form `"28:00.0"`). The BDF is also fed through
    /// `detail::sanitize_bdf_for_file_prefix` to derive the DPDK
    /// `--file-prefix` when `file_prefix` below is empty.
    std::string_view pci;

    /// Queues per process slot. Defaults to 1 — i.e. each MP peer
    /// owns exactly one RX queue. Set to >1 to give each peer a
    /// contiguous queue range (e.g. 4-queue NIC with
    /// `queues_per_proc=2` ⇒ 2 peers, each owning 2 queues).
    uint16_t queues_per_proc{1};

    /// Maximum number of process slots. `0` means auto-derive as
    /// `pcfg_template.nb_rx_queues / queues_per_proc`. Capped at
    /// `MpTopology::kMaxProcs` (= 64) by the registry.
    uint8_t max_procs{0};

    /// Optional override for the DPDK `--file-prefix`. Empty means
    /// derive from `pci` as `"eph_" + sanitize(pci)` so two peers
    /// pointing at the same BDF naturally agree on the prefix
    /// without sharing any state. Override only when you need to
    /// run multiple autojoin sessions on the same BDF (rare;
    /// usually a configuration mistake).
    std::string_view file_prefix{};

    // ─── PlatformConfig pass-through ─────────────────────────────────────

    /// Template PlatformConfig — every field that `Platform::create_*`
    /// understands flows through here unchanged. Autojoin only
    /// overrides three fields it must own at construction time:
    ///   - `proc_type`     resolved by `rte_eal_process_type()`
    ///                     after EAL init (Primary or Secondary)
    ///   - `mp_topology`   synthesized as
    ///                     `MpTopology::uniform(self_idx, max_procs,
    ///                                          nb_rx_queues)`
    ///   - `file_prefix`   set to the auto-derived (or
    ///                     caller-overridden) value
    ///
    /// Everything else (`port_id` / `nb_rx_queues` / `nb_tx_queues` /
    /// `mbuf_pool_size` / `per_lcore_pools` / `enable_promiscuous` /
    /// future fields) lands verbatim on the constructed Platform.
    /// **Required: `pcfg_template.nb_rx_queues > 0`** (autojoin
    /// derives `max_procs` from it).
    PlatformConfig pcfg_template{};

    // ─── EAL options ─────────────────────────────────────────────────────

    /// Typed lcore→cpu pin spec. When non-empty, autojoin validates
    /// each pin against the process-wide pin registry (SMT
    /// siblings / NUMA / IRQ / duplicate-cpu policy per `pin_policy`
    /// below) BEFORE `rte_eal_init` fires, so a misconfigured
    /// topology surfaces as a loud cold-path error. The validated
    /// pins are then lowered to a `--lcores=<lcore>@<cpu>,...` argv
    /// fragment. Pin ownership transfers to the returned Platform
    /// and is released when the Platform is destroyed.
    ///
    /// Mutually exclusive with `lcores` below.
    std::span<eph::dpdk::LcorePin const> pins{};

    /// Strictness of the typed-pin validator. Defaults to the same
    /// relaxed `CpuPinPolicy{}` every existing in-repo `pin_thread`
    /// caller uses. Has no effect when `pins` is empty.
    eph::utils::CpuPinPolicy pin_policy{};

    /// Raw lcore list (one entry per `-l` argument). Empty is
    /// allowed — DPDK uses its default. Autojoin does not auto-derive
    /// lcores; lcore consensus across processes is the caller's
    /// responsibility (the same problem the declarative path leaves
    /// to the caller). Mutually exclusive with `pins`.
    ///
    /// **Footgun**: identical to `EalConfig::lcores` — DPDK only
    /// honours the LAST `-l` token on the command line, so a
    /// multi-entry vector like `{"0", "1", "2"}` collapses to `-l 2`.
    /// To run on multiple lcores via this raw path, pass a SINGLE
    /// entry using DPDK list/range syntax (`{"0-3"}` or
    /// `{"0,2,4,6"}`). The typed `pins` path above sidesteps this
    /// by emitting a single `--lcores=...` token instead.
    std::vector<std::string> lcores{};

    /// Extra raw EAL argv tokens (e.g. `--log-level=lib.eal:warning`).
    /// Appended verbatim to the assembled argv after the typed
    /// transformations.
    std::vector<std::string> eal_extras{};

    /// Bitmask of EAL lcores **this process** owns (i.e. the lcores
    /// passed via `lcores` / `pins`). Default 0 = opt out of cross-
    /// process lcore conflict detection (back-compat for callers
    /// that haven't migrated). When non-zero, the synthesized
    /// `MpTopology::procs[self_index].lcore_mask` is set to this
    /// value, and `MpRegistryHandle::attach_secondary` rejects any
    /// peer whose mask overlaps with currently-claimed peers.
    /// Catches "two procs accidentally pinned to the same lcore →
    /// silent OS-scheduler CPU theft → p99 noise" mental-model gap.
    /// Capped at 64 bits (matches `MpTopology::kMaxProcs`).
    uint64_t self_lcore_mask{0};
};

} // namespace eph::dpdk
