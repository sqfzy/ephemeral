#pragma once

/// @file proc_type.hpp
/// Minimal cross-cutting header for `ProcType` — the DPDK process role
/// enum used by both `platform.hpp` (full Platform contract) and
/// `eal.hpp` (EAL argv assembly).
///
/// Lives in its own header so the enum has a single source of truth: a
/// future addition (e.g. `Auto`) automatically reaches every consumer
/// without the dual-source serialization hazard that motivated this
/// extraction (M3 of the MP-review fix sweep).

#include <cstdint>
#include <string_view>

namespace eph::dpdk {

/// @brief DPDK process role for single-NIC multi-process (primary+secondary).
///
/// Primary: full port lifecycle — enumerate, create mempool, configure /
///          start port, own RSS/RETA. Default for all existing code paths
///          (matches current single-process behavior byte-for-byte).
/// Secondary: attaches to an already-running primary via shared hugepage
///            (DPDK `--proc-type=secondary --file-prefix=<same as primary>`).
///            Looks up the primary's mempool by name; MUST NOT call
///            `rte_eth_dev_configure/start/stop/close` or
///            `rte_mempool_free` — these would corrupt the primary's
///            port state. Has its own Poller(s), Stream(s), FlowRule(s),
///            and ICMP registry (all per-process). The primary must
///            start first and outlive every secondary.
/// Auto: emits `--proc-type=auto`. DPDK then resolves at
///       `rte_eal_init` time: first peer to claim the
///       `--file-prefix` lockfile becomes primary, every later
///       peer auto-attaches as secondary. The actual role is
///       reported by `rte_eal_process_type()` after init.
///       Used exclusively by `Platform::join_dynamic` (the
///       autojoin path); declarative-path callers must pass
///       Primary or Secondary explicitly.
///
/// See `docs/dpdk-multiprocess.md` for startup/teardown ordering,
/// queue/src_port segmentation rules, and PMD-specific caveats.
enum class ProcType : uint8_t { Primary, Secondary, Auto };

/// @brief Serialize a ProcType to the string DPDK expects after
///        `--proc-type=`. Single source of truth — both `EalConfig`
///        argv assembly and any logging path must call this rather than
///        recomputing the mapping.
[[nodiscard]] constexpr std::string_view
to_eal_string(ProcType p) noexcept {
    switch (p) {
        case ProcType::Primary:   return "primary";
        case ProcType::Secondary: return "secondary";
        case ProcType::Auto:      return "auto";
    }
    // Unreachable in well-formed code; returning "primary" is the
    // conservative default (DPDK's auto behavior). If a future enum
    // value is added without updating this switch, GCC's
    // -Wreturn-type / -Wswitch will flag it at compile time.
    return "primary";
}

// ── Compile-time guards ──
// These keep the underlying type narrow (so embedding ProcType in
// per-stream / per-mempool config structs is byte-cheap) and lock the
// EAL string contract — DPDK matches `--proc-type=` arguments by exact
// string equality, so a typo here would silently degrade a primary into
// auto-mode at startup. Caught at compile time, not at runtime.
static_assert(sizeof(ProcType) == 1,
              "ProcType must remain 1 byte — embedded in hot config structs");
static_assert(to_eal_string(ProcType::Primary)   == "primary",
              "EAL --proc-type=primary spelling drift would break MP startup");
static_assert(to_eal_string(ProcType::Secondary) == "secondary",
              "EAL --proc-type=secondary spelling drift would break MP startup");
static_assert(to_eal_string(ProcType::Auto)      == "auto",
              "EAL --proc-type=auto spelling drift would break autojoin startup");

} // namespace eph::dpdk
