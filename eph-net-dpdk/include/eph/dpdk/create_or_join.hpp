#pragma once

/// @file create_or_join.hpp
/// `CreateOrJoinConfig` — the user-facing config for the **autojoin**
/// MP factory (`Platform::create_or_join`). Autojoin is the
/// zero-coordination multi-process bring-up: two unrelated processes
/// that share a NIC agree on nothing except the PCI BDF and
/// `nic.nb_rx_queues`. Whoever `rte_eal_init`s first
/// becomes primary; later peers attach as secondaries and CAS-claim
/// the next free process slot themselves.
///
/// **Mental model**: `CreateOrJoinConfig` only carries
/// **autojoin-specific** inputs — everything `PlatformConfig` already
/// has (per_lcore_pools / mbuf_pool_size / port_id / nb_rx_queues /
/// every other field) flows through `nic`. Autojoin only
/// overrides three fields it must own: process role (resolved at EAL
/// init), MpTopology (synthesized from claimed slot), and `file_prefix`
/// (auto-derived from BDF).
///
/// Autojoin is the **only** multi-process entry point — the
/// cooperative path (`Platform::attach` + a shared file_prefix /
/// hand-written EalConfig) was removed. autojoin's contract:
///   - file_prefix is auto-derived from the BDF (so two peers naming
///     the same NIC name the same hugepage segment without sharing
///     a string)
///   - max_procs is auto-derived from `nic.nb_rx_queues /
///     nic.queues_per_proc`
///   - self_index is auto-claimed at attach time (CAS-strong against
///     the registry's `procs[].claimed` flags)
/// The result is *no protocol* between peers — both run the same
/// binary with the same args; whoever wins the EAL race becomes
/// primary.
///
/// Hot path: NONE. `Platform::create_or_join` is the cold setup factory;
/// once the Platform is up, the runtime path is byte-for-byte
/// identical to a declarative-path Platform with the same self_index.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "eph/dpdk/lcore_pin.hpp"   // LcorePin (typed pin spec)
#include "eph/utils/cpu.hpp"        // CpuPinPolicy

// This header references `eph::dpdk::PlatformConfig` (`nic`
// is a value member) but cannot directly `#include
// "eph/dpdk/platform.hpp"` without a circular include — platform.hpp
// includes this header from inside its own body, after PlatformConfig
// is fully parsed. The contract: include `eph/dpdk/platform.hpp` to
// get `CreateOrJoinConfig`. Users who try to include this header
// standalone get a loud diagnostic instead of a confusing
// "PlatformConfig has not been declared".
#ifndef EPH_DPDK_PLATFORM_CONFIG_DEFINED
#  error "Include eph/dpdk/platform.hpp instead of create_or_join.hpp directly: " \
         "CreateOrJoinConfig embeds a PlatformConfig and must be parsed " \
         "after PlatformConfig is fully defined."
#endif

namespace eph::dpdk {

/// @brief Zero-consensus autojoin config.
///
/// Required: `pci`. Everything else has a sensible default.
/// **Secondary peers only need `pci` (and per-peer EAL args).**
struct CreateOrJoinConfig {
    /// PCI BDF of the NIC to attach. Drives BOTH the EAL `-a` allowlist
    /// AND the auto-derived `file_prefix` (`"eph_" + sanitize(pci)`).
    /// Two peers naming the same BDF naturally agree on hugepage
    /// namespace without sharing any string.
    std::string_view pci;

    /// Primary-side full PlatformConfig — used ONLY when this peer
    /// resolves to primary (first to call `eal_init`). Secondary peers
    /// have every field of this struct ignored except as fallback
    /// defaults; the library reads NIC physical state from the live
    /// device after attach. The `max_procs` / `queues_per_proc` knobs
    /// live here, not at the top level, so secondary callers can't
    /// accidentally specify them.
    /// **Required when this peer resolves to primary**:
    /// `nic.nb_rx_queues > 0`. (Secondary peers may leave
    /// at default — the autojoin path queries the live NIC.)
    PlatformConfig nic{};

    // ─── EAL options ─────────────────────────────────────────────────────
    // Per-peer (not consensus-bearing).

    /// Typed lcore→cpu pin spec. When non-empty, autojoin validates
    /// each pin against the process-wide pin registry (SMT siblings /
    /// NUMA / IRQ / duplicate-cpu policy per `pin_policy` below) BEFORE
    /// `rte_eal_init` fires, so a misconfigured topology surfaces as a
    /// loud cold-path error. The validated pins are then lowered to a
    /// `--lcores=<lcore>@<cpu>,...` argv fragment. Pin ownership
    /// transfers to the returned Platform and is released when the
    /// Platform is destroyed.
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
    std::vector<std::string> extra_eal_args{};

    /// Bitmask of EAL lcores **this process** owns (i.e. the lcores
    /// passed via `lcores` / `pins`). Default 0 = opt out of cross-
    /// process lcore conflict detection. When non-zero, the synthesized
    /// `MpTopology::procs[self_index].lcore_mask` is set to this
    /// value, and `MpRegistryHandle::attach_secondary` rejects any
    /// peer whose mask overlaps with currently-claimed peers.
    /// Catches "two procs accidentally pinned to the same lcore →
    /// silent OS-scheduler CPU theft → p99 noise" mental-model gap.
    /// Capped at 64 bits (matches `MpTopology::kMaxProcs`).
    /// Single source of truth: this is the only `self_lcore_mask` —
    /// `PlatformConfig` deliberately does not carry a duplicate.
    uint64_t self_lcore_mask{0};
};

} // namespace eph::dpdk
