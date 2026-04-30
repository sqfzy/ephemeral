#pragma once

/// @file join_dynamic.hpp
/// `JoinDynamicConfig` — the user-facing config for the **autojoin**
/// MP factory (`Platform::join_dynamic`). Autojoin is the
/// zero-coordination multi-process bring-up: two unrelated processes
/// that share a NIC agree on nothing except the PCI BDF and
/// `nb_rx_queues`. Whoever `rte_eal_init`s first becomes primary;
/// later peers attach as secondaries and CAS-claim the next free
/// process slot themselves.
///
/// Compared to the **declarative** path (`Platform::create_primary` /
/// `create_secondary` + a hand-written `MpTopology`), autojoin
/// trades per-peer slot precision for *no protocol* between peers:
///   - file_prefix is auto-derived from the BDF (so two peers naming
///     the same NIC name the same hugepage segment without sharing
///     a string)
///   - max_procs is auto-derived from `nb_rx_queues / queues_per_proc`
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
#include <string>
#include <string_view>
#include <vector>

namespace eph::dpdk {

/// @brief Autojoin (`Platform::join_dynamic`) configuration.
///
/// Required fields: `pci`, `nb_rx_queues`. Everything else is
/// auto-derived or has a sensible default that callers rarely need
/// to override.
struct JoinDynamicConfig {
    /// PCI BDF of the NIC to attach (e.g. `"0000:28:00.0"` or short
    /// form `"28:00.0"`). The BDF is also fed through
    /// `detail::sanitize_bdf_for_file_prefix` to derive the DPDK
    /// `--file-prefix` when `file_prefix` below is empty.
    std::string_view pci;

    /// Total RX rings the primary will configure on the port. All
    /// peers must agree on this value because it determines how the
    /// queue space is partitioned across `max_procs` process slots.
    uint16_t nb_rx_queues{0};

    /// Queues per process slot. Defaults to 1 — i.e. each MP peer
    /// owns exactly one RX queue. Set to >1 to give each peer a
    /// contiguous queue range (e.g. 4-queue NIC with
    /// `queues_per_proc=2` ⇒ 2 peers, each owning 2 queues).
    uint16_t queues_per_proc{1};

    /// Maximum number of process slots. `0` means auto-derive as
    /// `nb_rx_queues / queues_per_proc`. Capped at
    /// `MpTopology::kMaxProcs` (= 64) by the registry.
    uint8_t max_procs{0};

    /// Optional override for the DPDK `--file-prefix`. Empty means
    /// derive from `pci` as `"eph_" + sanitize(pci)` so two peers
    /// pointing at the same BDF naturally agree on the prefix
    /// without sharing any state.
    std::string_view file_prefix{};

    /// Lcore list (one entry per -l argument). Autojoin does not
    /// auto-derive lcores: lcore consensus across processes is the
    /// caller's responsibility (the same problem the declarative
    /// path leaves to the caller). Empty is allowed — DPDK then
    /// uses its default.
    std::vector<std::string> lcores{};

    /// Target DPDK port id once the EAL has bound the device under
    /// the auto-derived file_prefix. Almost always `0` for a single-
    /// device process.
    uint16_t port_id{0};
};

} // namespace eph::dpdk
