# eph-net-dpdk changelog

## [Unreleased]

### Added — bench infra: `lat all --dpdk` parallel multi-scenario runner

A new entry point in `benchmarks/latency/lat`: `lat all --dpdk`
brings up a single Platform sharing NIC_B across N concurrent EAL
worker lcores, each running one lat scenario from a new
`[parallel].runs[]` config section. ~7× faster end-to-end DPDK
verification (single-binary serial: 7 × 30s = 3.5min → multi: ~30s).

```toml
[parallel]
runs = [
  { scenario = "lat_tcp",       lcore = 1, cpu = 4, queue = 0 },
  { scenario = "lat_udp",       lcore = 2, cpu = 5, queue = 1 },
  { scenario = "lat_ws",        lcore = 3, cpu = 6, queue = 2 },
  { scenario = "lat_ex_market", lcore = 4, cpu = 7, queue = 3 },
  ...
]
```

Implementation (parallel-bench v2 reshape): per-scenario inner
measurement loops extracted to `benchmarks/latency/scenarios/
<name>_loop.hpp` as reusable `run_lat_<sc>_loop<EnableTls>(BenchCtx&)`.
A new orchestrator `lat_multi_dpdk` (modeled on
`examples/simple_hft_dpdk_rss.cpp`'s worker pattern) constructs N
`BenchCtx` (one per `runs[]` row), registers a Poller per RX queue
with the Platform, then `rte_eal_remote_launch`-es each worker
lcore to a `switch (scenario_id)` dispatch into the appropriate
inner loop. Each scenario's inbound traffic Toeplitz-hashes to its
owned queue via `pin_to_queue` (Task 1 RSS-aware connect fix).

Per-slot JSON outputs use `_slot<i>` suffix; single-binary commands
(`lat tcp --dpdk` etc.) keep `slot_index = -1` and produce
byte-identical output (no suffix) — verified with 30s parity gate
≤ 5%.

`eph-net-dpdk` library code is unchanged — this is application-layer
plumbing on top of the existing `Platform::create_with_eal` +
`LcorePin[]` + `register_poller` + RSS-aware
`create_and_attach(pin_to_queue=...)` infrastructure.

### Fixed — `DpdkTcpStream::create_and_attach` RSS-aware connect (BREAKING)

`create_and_attach` in `RxDispatchMode::RssPartitioned` previously
had two branches:

  - `pin_to_queue` set       → engineer src_port via
                                `find_src_port_for_queue` so SYN-ACK
                                Toeplitz-hashes to the pinned queue.
  - `pin_to_queue` unset     → defer queue selection: pick `target_qid`
                                via the rr_counter, run `create()` with
                                the caller's *original* src_port, then
                                post-create call `predict_rss_queue` on
                                the final 5-tuple to figure out which
                                queue would receive future packets.

The deferred path left the SYN's src_port untouched. Under
multi-process autojoin (`Platform::join_dynamic`) where each peer
owns only a subset of RX queues, the SYN-ACK's RSS hash routed it
to whichever queue the caller's arbitrary src_port hashed to —
typically owned by *another* process, which had no `TcpSession` for
this 5-tuple and silently dropped the packet. Manifested as: an
autojoin secondary's `lat_tcp_dpdk` client hanging at TCP connect,
zero samples recorded over a 30s window. Single-process callers
were unaffected because all queues belonged to one Platform.

The two branches are now merged: `RssPartitioned` *always* engineers
src_port via `find_src_port_for_queue`, regardless of how
`target_qid` was determined. `defer_queue_selection` and the
post-create `predict_rss_queue` block are deleted; `target_qid` is
locked in *before* `create()` runs, so the handshake polls the
correct queue from the very first SYN.

**BREAKING CHANGE**: in `RssPartitioned` mode, caller-set
`cfg.dpdk.tcp_low_level.tuple.src_port` is no longer preserved
across `create_and_attach`. The library always selects an
RSS-aligned src_port. `StreamSnapshot::Endpoint::src_port_rewritten`
correctly reports this. Audit of every in-tree caller (5 examples +
lat_tcp + 9 e2e) found zero callers depend on src_port preservation
across `create_and_attach`.

Operational note: when `pin_to_queue` is unset and the autojoin /
mp_topology partition gives this process a non-zero queue range,
remember to set `nb_tx_queues = nb_rx_queues` (or any value ≥
`target_qid + 1`) so the engineered TX queue id maps to a real
TX ring. Default `nb_tx_queues = 1` only works for single-queue
or queue-0-pinned scenarios.

`Software` and `FlowDirector` branches are unchanged — `Software`'s
single-queue mode never needed src_port engineering; `FlowDirector`
already had a separate KNOWN LIMITATION around handshake-race
timing (tracked under its own reshape).

Verification on this commit:
  - 11 unit suites / 186 cases — PASS
  - 6 e2e (5 existing + new `dpdk_mp_dynamic_tcp_handshake_e2e`) — PASS
  - lat_tcp_dpdk 30s parity: p50 +0.8% / p99 +4.9% (≤5% gate)
  - lat_udp_dpdk 30s parity: p50 +0.8% / p99 +3.5% (≤5% gate)

### Added — `Platform::create_with_eal` (unified one-call factory)

The new top-level user-facing factory: brings up EAL + Platform in
a single call, and the returned Platform **owns the EAL session**.
`~Platform` releases DPDK resources, then runs `eal_cleanup`
atomically. No more `EalGuard + Platform::create_X` two-step
pattern; no more `{ auto _drop = std::move(plat); } eal_cleanup();`
idiom.

```cpp
auto plat = eph::dpdk::Platform::create_with_eal(
    pcfg, eal_cfg,
    pins, policy);
// ~Platform at scope exit → DPDK teardown → pin-guards release →
// eal_cleanup, in that order, atomically.
```

Mental model: **Platform is the root of DPDK + EAL ownership**.
A factory's job is to build that root; how much it does for you
is the only difference between the three options:

| Factory | EAL ownership | Use when |
|---------|---------------|----------|
| `Platform::create(pcfg)` | caller-managed (e.g. EalGuard) | sharing one EAL across multiple Platforms; legacy |
| `Platform::create_with_eal(pcfg, eal_cfg, pins, policy)` | Platform owns EAL | almost every example / bench / production single-process + declarative MP |
| `Platform::join_dynamic(JoinDynamicConfig)` | Platform owns EAL | autojoin (zero-coordination MP) |

### Changed — `Platform::Impl` extends EAL-ownership state (BREAKING)

**BREAKING CHANGE**: This is an internal-only struct change but
worth noting:
  - Added `std::vector<eph::utils::PinGuard> pin_session_guards;`
  - Added `bool owns_eal_init{false};`
  - `~Platform()` is now explicit (was `= default`); manually
    `impl_.reset()` first, then runs `eal_cleanup` if
    `owns_eal_init`. The order is critical: with the default
    destructor, ~Impl's body runs BEFORE Impl's fields destruct,
    so `eal_cleanup` would have torn down hugepage memzones that
    `mp_registry` / `icmp_directory` field destructors then
    accessed → SEGV. The explicit body sequences these correctly.

### Changed — `JoinDynamicConfig` collapsed via pcfg_template (BREAKING)

**BREAKING CHANGE**: `JoinDynamicConfig` no longer accepts
PlatformConfig fields at top level. Single source of truth is
`pcfg_template`:

```cpp
// Before:
Platform::join_dynamic({
    .pci          = "0000:28:00.0",
    .nb_rx_queues = 4,         // top-level
    .port_id      = 0,         // top-level
    .lcores       = {"0,1"},
});

// After:
Platform::join_dynamic({
    .pci           = "0000:28:00.0",
    .pcfg_template = {.nb_rx_queues = 4},  // any PlatformConfig field
    .lcores        = {"0,1"},
});
```

Removed top-level fields: `nb_rx_queues`, `port_id`. Removed
unused: legacy `lcores` was already at top level — kept (it's an
EAL-level option).

Added: `pcfg_template` (PlatformConfig embedded by value), `pins`
+ `pin_policy` (typed-pin path), `eal_extras` (raw EAL argv
passthrough).

Migrating: replace `cfg.nb_rx_queues = N` with
`cfg.pcfg_template.nb_rx_queues = N`. Replace `cfg.port_id = X`
with `cfg.pcfg_template.port_id = X`. Other PlatformConfig
fields (`per_lcore_pools`, `mbuf_pool_size`,
`enable_promiscuous`, etc.) — same pattern.

### Changed — `DpdkBenchEnv` API collapsed to one factory (BREAKING)

**BREAKING CHANGE**: `eph::dpdk::test::DpdkBenchEnv` (used by
benchmarks and integration test fixtures):
  - Removed `create_full(int argc, char** argv, ...)` — both
    overloads.
  - Removed `create_full_with_pins(eal_cfg, pins, policy, ...)`.
  - Added `create(PlatformConfig pcfg, EalConfig eal_cfg,
                  std::span<LcorePin const> pins,
                  CpuPinPolicy pin_policy,
                  mock_ip, client_ip, gateway_ip)`.
  - Removed `eph::dpdk::EalGuard eal` field on the struct.
    Platform owns EAL via `create_with_eal` now;
    `~DpdkBenchEnv` → `~Platform` → eal_cleanup chains
    automatically.

Migrating: replace `create_full(argc, argv, ...)` with
`create(PlatformConfig{...}, EalConfig{...}, /*pins=*/{},
CpuPinPolicy{}, mock_ip, client_ip, gateway_ip)`. The 6-arg argv
splitter is gone — pass EalConfig directly with `program_name`,
`allowed_devs`, `lcores`, etc.

### Changed — every example + integration test migrated

Every user-facing example and integration test binary now uses the
unified factory:
  - **Examples migrated to `Platform::create_with_eal`**:
    `simple_hft_dpdk_rss`, `dpdk_multicast_md`, `binance_latency`,
    `simple_hft_dpdk_mp` (all 4 — last reshape's
    `simple_hft_dpdk_mp_dynamic` already used `join_dynamic`).
  - **Examples NOT migrated (deliberately)**:
    `simple_hft_dpdk` (smoke-boot demo, no Platform),
    `multi_port_platform_demo` (uses MultiPortPlatform aggregator),
    `async_dns_multi_resolve` (no Platform — DPDK Poller only).
  - **Integration tests migrated**: `dpdk_mp_primary` /
    `dpdk_mp_secondary` and the topology / icmp / fd_fallback
    pairs (8 binaries total).
  - **Tests using `DpdkBenchEnv`**: `test_dpdk_rss_platform`,
    `test_dpdk_rss_fanout`, `test_dpdk_e2e` (via
    `dpdk_e2e_env.hpp`).
  - **Bench infra**: `benchmarks/latency/core/dpdk_env.hpp` —
    `load_dpdk_env` now calls `DpdkBenchEnv::create`. lat_*_dpdk
    binaries automatically pick up the new path. Bench parity
    verified ≤ 5% on lat_tcp_dpdk + lat_udp_dpdk p50/p99.

The `EalGuard::init` / `init_with_pins` public API is **preserved
unchanged** as the advanced path for callers that need to share
one EAL across multiple Platform constructions (typical in unit
test fixtures). It is NOT marked `[[deprecated]]`.

### Added — `Platform::join_dynamic` (autojoin MP factory)

Zero-coordination multi-process bring-up. Two unrelated
processes sharing the same NIC can now stand up a primary +
secondary without any shared file_prefix string, manual
`MpTopology`, or explicit `self_index`:

```cpp
auto platform = eph::dpdk::Platform::join_dynamic({
    .pci          = "0000:28:00.0",
    .nb_rx_queues = 4,
});
// First peer  → primary (slot 0)
// Second peer → secondary (CAS-claims slot 1)
```

What `join_dynamic` does internally (all cold-path):

1. Validates `JoinDynamicConfig` (`pci` non-empty,
   `nb_rx_queues > 0`, `queues_per_proc > 0`).
2. Auto-derives the DPDK `--file-prefix` as
   `"eph_" + sanitize(pci)` when the caller leaves it empty
   (so two peers naming the same NIC name the same hugepage
   segment without sharing any string).
3. Auto-derives `max_procs` as `nb_rx_queues / queues_per_proc`.
4. Assembles EAL argv with `--proc-type=auto`, calls `eal_init`.
5. Asks DPDK which role this process resolved to via
   `rte_eal_process_type()`. First peer to claim the
   `--file-prefix` lockfile is primary; later peers auto-attach
   as secondary.
6. Primary path: synthesizes a uniform
   `MpTopology(self_index=0)` and delegates to
   `Platform::create_primary` — no behaviour change vs. the
   declarative path with the same topology.
7. Secondary path: `attach_secondary_readonly` to validate the
   primary's view, `try_claim_free_slot` for the lowest free
   `procs[].claimed` flag. Synthesizes a uniform `MpTopology`
   pointing at the claimed slot, transfers ownership via the
   new `attach_secondary(..., already_claimed=true)` bypass,
   and delegates to a new private `create_secondary_impl_`.

The hot path is unchanged: by the time the Platform is
returned, its `Impl` is byte-for-byte identical to one produced
by the declarative factories with the same self_index. `inc_<M>`,
`rr_counter`, FlowDir, ICMP, Poller are all per-process.

Supporting public surface:

- `eph::dpdk::JoinDynamicConfig` (in
  `eph/dpdk/join_dynamic.hpp`) — POD config: `pci` +
  `nb_rx_queues` required; `queues_per_proc` (default 1),
  `max_procs` (0 = auto), `file_prefix` (empty = auto from
  BDF), `lcores`, `port_id`.
- `eph::dpdk::ProcType::Auto` — new enum value, serializes to
  `--proc-type=auto`. `to_eal_string` static_assert coverage
  extended.
- `eph::dpdk::detail::sanitize_bdf_for_file_prefix` (in
  `eph/dpdk/detail/bdf_sanitize.hpp`) — validates a PCI BDF
  (length 7-12, hex digits + `:` + `.`, requires both
  separators) and returns the `_`-substituted form.
- `MpRegistryHandle::attach_secondary_readonly(file_prefix)` —
  view-only attach: look up + validate magic / version /
  file_prefix, but do NOT CAS-claim any slot.
- `MpRegistryHandle::try_claim_free_slot()` — scan
  `procs[0..total)` and CAS-claim the lowest free; returns
  `OutOfMemory` when full. Lock-free.
- `MpRegistryHandle::attach_secondary(..., bool
  already_claimed = false)` — defaulted parameter so existing
  callers compile unchanged. When `true` the helper trusts the
  caller's preclaim and skips the CAS, but still verifies the
  slot is in `claimed=1` state.
- `MpRegistryHandle::disarm_slot()` — drop slot ownership
  without clearing the shared-memory `claimed` flag; used to
  hand off ownership from the read-only handle to the full
  handle returned by `attach_secondary(already_claimed=true)`.

The declarative path (`Platform::create_primary` /
`create_secondary` + `MpTopology`) is preserved unchanged for
asymmetric topologies, tagged `self_index` control, and
callers already managing their own EAL bootstrap. The TL;DR in
`docs/dpdk-multiprocess.md` now leads with autojoin; the
declarative path is documented as "Advanced usage:
declarative topology" — same content, demoted but **not**
deprecated.

End-to-end demo: `examples/simple_hft_dpdk_mp_dynamic.cpp`
(run the same binary twice in two terminals; whichever calls
`eal_init` first becomes primary).

### Added — FlowDir secondary fallback via primary IPC

When `rte_flow_create` rejects a secondary's install (some PMDs
don't allow flow rule install from non-primary processes), the
library now auto-routes the install through DPDK's `rte_mp_*`
IPC to the primary, which installs the rule on the secondary's
behalf and returns an opaque handle id. From user code,
`Stream::create_and_attach` still "just works" — PMD compatibility
moves from caller's concern to library detail.

Implementation:
* `FlowRule::handle` evolved (stage 5) into `std::variant<
  monostate, LocalFlowHandle{rte_flow*}, RemoteFlowHandle{
  owner_proc, handle_id}>`. RAII destructor visit-dispatches:
  Local → `rte_flow_destroy`, Remote → `eph_fd_destroy` IPC.
  New `FlowRule::opaque_handle_id() -> uint64_t` accessor for
  telemetry; `to_json()` gains `"origin": "local" / "remote"
  / "none"`.
* New POD wire formats: `FdInstallMsg` / `FdInstallReply` /
  `FdDestroyMsg` / `FdDestroyReply`, all version-tagged for
  cross-build compatibility.
* Primary-side: `detail::RemoteFlowRulesMap` (mutex-guarded
  unordered_map<handle_id, {port, rte_flow*}> + atomic counter)
  stores rules installed on behalf. `on_fd_install_thunk` /
  `on_fd_destroy_thunk` are static `rte_mp_t` handlers
  registered by `Platform::create_primary`. ~Impl
  `destroy_all()` GCs any rules a secondary couldn't tear down.
* Secondary-side: `try_install_flow_rule_via_ipc(port, queue,
  tuple, proto, owner_proc=0)` packs an FdInstallMsg, fires
  `mp_ipc_request_sync` (5 s timeout), validates the reply,
  wraps in a `FlowRule(RemoteFlowHandle{...})`. `Stream::create_
  and_attach` (TCP + UDP) gates on `is_secondary() && has_mp_
  topology()` and falls through to this on local
  rte_flow_create rejection.

Behaviour invariants:
* Single-process / primary-mode / non-mp_topology Platforms:
  byte-for-byte unchanged. The fallback gate is false in those
  contexts, so the IPC path is dead code.
* PMDs that support secondary install: zero IPC overhead. The
  local rte_flow_create succeeds and the variant holds a
  LocalFlowHandle, exact same end-to-end behaviour as the
  pre-variant FlowRule.

Hot path: zero touched. All four new code paths
(install via IPC, destroy via IPC, primary-side install
handler, primary-side destroy handler) are cold. Stream
attach / detach is the one cold-path call site that takes the
extra IPC roundtrip when the fallback fires.

`docs/dpdk-multiprocess.md` PMD compat table updated; the "If
your PMD rejects `rte_flow_create` in secondary, file an issue"
note replaced with an "auto-handled" pointer to a new
"FlowDir secondary fallback" section. New e2e binaries
`dpdk_mp_fd_fallback_primary` / `dpdk_mp_fd_fallback_secondary`
plus `dpdk_mp_fd_fallback_e2e.sh` exercise the IPC bidirectional
channel; on hosts where the PMD allows primary-side
rte_flow_create the destroy round-trip is also exercised, on
PMD-limited hosts (ENA in RSS-active mode returns ENOSYS) the
test passes with handler-path-only coverage and an INFO log.

### Added — Cross-process ICMP MTU propagation (auto-forwarding)

When `cfg.mp_topology` is set, ICMP Frag Needed messages landing on
a peer process's RX queue are now auto-forwarded to the owning
process via DPDK's `rte_mp_*` IPC. Pre-reshape behaviour: a Frag
Needed on a non-owner queue silently drops, the owning stream's
`effective_mss` never shrinks, and the resulting "router fires
Frag Needed → we send oversized → router fires again" loop produces
hard-to-diagnose packet storms. After this change: the receiving
process consults a hugepage-backed `IcmpDirectory` to find the
owner, fires a one-way `eph_icmp_dispatch` IPC msg, the owner's
`IcmpRegistry::dispatch` runs as if the message had arrived locally,
and `TcpSession::on_icmp_frag_needed` shrinks `effective_mss`
correctly.

API surface: zero changes to user code. `Platform::register_icmp_
target` still returns `IcmpTargetHandle`; the alias now points at a
new `IcmpTargetCompoundHandle` that internally wraps both the
existing `IcmpRegistry::Handle` and an `IcmpDirectorySlotGuard`.
Move-only, RAII destruct in the right order (directory side first,
which bumps generation, so any in-flight peer forward observing the
old gen drops as stale before the local target slot is freed).

Implementation:
* `detail/mp_ipc.hpp` — typed RAII wrapper around `rte_mp_*`
  (`MpIpcAction`, `mp_ipc_send_oneway<T>`, `mp_ipc_request_sync<T,
  R>`, `pack_msg<T>` / `parse_payload<T>`). Degrade-on-failure: if
  EAL rejects action registration, the handle reports `bool ==
  false` and call sites skip IPC paths.
* `detail/icmp_directory.hpp` — POD `IcmpDirectoryEntry` (atomic
  claimed + 4-tuple + proto + owner_proc + atomic generation),
  cacheline-aligned `IcmpDirectoryHeader` with `magic`/`version`/
  per-slot generation + atomic stats counters
  (`ipc_msgs_sent`/`ipc_msgs_received`/`dropped_stale`/
  `dropped_no_owner`). 1024 slots cap (~4 procs × 256 streams). The
  `on_icmp_dispatch_thunk` is the static `rte_mp_t` handler that
  receives forwarded msgs, gen-checks, and dispatches into the
  local `IcmpRegistry`.
* `detail/icmp_registry.hpp` — added `dispatch_returns_hit(parsed)
  -> bool` (existing `void dispatch` unchanged for invariant). Used
  by the Poller closure to decide local-hit vs cross-proc forward.

Hot path: zero changes. Every step is cold path (Platform
bring-up, stream attach, ICMP miss path, IPC handler thread).
Verified by force-rebuild bench: `lat_tcp_dpdk` p50 21.6 µs (vs
baseline 22.2 µs, -3 %), `lat_udp_dpdk` p50 19.6 µs (vs 19.6 µs,
0 %).

Degrade-on-failure: `rte_mp_action_register` failure (e.g. EAL
`--no-shconf`) → `SPDLOG_ERROR` + continue, cross-proc forwarding
silently falls back to per-process drop (= pre-reshape behavior).

Restart contract: `Platform::create_primary` resets the
`eph_mp_icmp/<file_prefix>` memzone (clears stale entries from a
previous run). Same operational rule as the existing MpRegistry —
stop every secondary before restarting the primary. Per-slot
`generation` (uint32) drops in-flight stale forwards even within a
single primary's lifetime.

`docs/dpdk-multiprocess.md` adds a "Cross-process ICMP MTU
propagation" section; the corresponding `out of scope` line is
removed. New e2e binaries `dpdk_mp_icmp_primary` / `dpdk_mp_icmp_
secondary` plus `dpdk_mp_icmp_e2e.sh` exercise the full forward
round trip.

### Added — `MpTopology` + shared registry: auto-derived MP resource layout

Multi-process resource allocation now needs only `(self_index,
total_procs)` for the typical uniform case. The new
`eph::dpdk::MpTopology` value type (`include/eph/dpdk/mp_topology.hpp`)
declares the full per-process layout (RX queue range + src_port
window) and feeds it into `PlatformConfig::mp_topology`. At
`Platform::create_primary` time the library reserves a
hugepage-backed shared memzone (`eph_mp/<file_prefix>` — see
`include/eph/dpdk/detail/mp_registry.hpp`), writes the topology, and
CAS-claims this process's slot. `create_secondary` looks up the same
memzone, cross-validates magic / version / file_prefix / total_procs
/ per-slot self spec, and CAS-claims its own slot — two processes
declaring the same `self_index` lose CAS and get a clear
`InvalidConfig` diagnostic instead of silently corrupting each
other's state.

`Platform` gains two cold-getter accessors:
  - `has_mp_topology()` — true iff this process attached the registry.
  - `self_port_range()` — `std::optional<{port_lo, port_hi}>` that
    `DpdkTcpStream::create_and_attach` / `DpdkUdpSocket::
    create_and_attach` consult on the RSS-pinned path to narrow
    `find_src_port_for_queue`'s search to the per-process window. The
    helper's own signature is unchanged.

The legacy hand-partitioned `rx_queue_range` + caller-allocated
src_port path is fully preserved as the "Advanced usage" escape
hatch — `mp_topology` left empty leaves every existing behaviour
byte-for-byte identical, including the `{0, 0}` sentinel "full
range" semantics. `validate_config` rejects setting both
`mp_topology` and a non-sentinel `rx_queue_range` (the two paths
must not have two sources of truth).

Restart contract: `create_primary` always resets the registry. The
operational rule "stop every secondary before restarting the
primary" is unchanged — same contract DPDK already imposes for the
shared mempool.

`docs/dpdk-multiprocess.md` rewritten so the recommended path is the
TL;DR; the legacy fields are demoted to "Advanced usage: manual
partitioning". `examples/simple_hft_dpdk_mp.cpp` switched to drive
`MpTopology::uniform`. New e2e binaries `dpdk_mp_topology_primary`
/ `dpdk_mp_topology_secondary` plus orchestrator
`tests/integration/dpdk_mp_topology_e2e.sh` exercise the full
primary↔secondary cycle. Existing `dpdk_mp_e2e.sh` (legacy
hand-partition path) and the 22-case
`test_dpdk_multiprocess_config` suite remain unchanged and continue
to pass — invariants verified.

### Fixed — `parse_arp_reply` reflection-attack mitigation

`parse_arp_reply` previously only validated `sender_ip == target_ip`
(the IP being resolved). An attacker sniffing our broadcast could
craft a reply with the real gateway's IP/MAC in the sender slots but
with the reply's `target_ip` field pointing at a different host (or
zero) — the parser accepted it, letting the attacker poison our cache
with the gateway's MAC at any time and bypassing the `expected_mac`
allowlist gate (which only checks sender MAC, not the reply target).

Add an optional `expected_local_ip` parameter (default nullopt to
preserve the fuzzer harness's permissive shape) and have
`resolve_with_io` pass our `src_ip`. Mismatched target_ip is logged
WARN and rejected. The previous batch's `parse_arp_reply` doc note
already described the limitation; this entry closes the gap rather
than just documenting it.

### Fixed — `TcpSession::send_batch` reports caller's original count

Pre-fix, `send_batch` clamped the input count to `kMaxBatchSize=32`
silently — a caller passing 50 segments saw `BatchSendResult{32, 32}`
on a clean burst, indistinguishable from a clean full success. The
silent loss of segments 32..49 only surfaced through downstream
sequence-number drift. Snapshot the caller's original count at entry,
return it as `requested`, and emit a WARN at the clamp point. Aligns
the over-batched case with the partial-tx_burst case (NIC
backpressure), which already truthfully reports `sent < requested`.

### Docs — `parse_arp_reply` security note refreshed

The pre-existing `@note Security` comment on `eph::dpdk::arp::
parse_arp_reply` claimed the function "does NOT detect ARP spoofing",
but the implementation grew sender-MAC well-formedness checks
(reject all-zero / non-unicast I-G bit) and an optional `expected_mac`
allowlist over the past months. The note now lists what we validate
(opcode / hw+proto types / target IP match / sender MAC well-formed /
optional expected_mac) and what we still don't (reflection-style
attacks where reply.target_ip != our local IP), and calls out the
recommended HFT colo posture (pre-configured gateway MAC + expected_mac).

### Fixed — `UdpConfig::warnings` flags `src_ip == dst_ip` self-send

`TcpConfig::warnings` already surfaces the self-connect case
(`tuple.src_ip == tuple.dst_ip`); the matching `UdpConfig::warnings`
did not, so a misconfigured UDP socket aimed at the local IP went
silently to the wire (DPDK has no kernel-loopback fallback) and the
peer never received the frame. Add the same advisory to UDP. Two
existing `WarningsHwCksum` / `WarningsEmptyNoHwCksum` tests had been
silently passing because the prior advisories were already firing
on default-zero MACs — populate non-zero MACs so the size-1 / empty
expectations actually exercise the documented contract.

### Fixed — `EalGuard::init_with_pins` rejects raw `--lcores` in `extra_args`

`init_with_pins` already rejected the `cfg.lcores` raw escape hatch
when typed pins were also supplied (mutually exclusive), but the
identical duplication is reachable through `cfg.extra_args` (where
users hand-write argv tokens). DPDK silently keeps only the *last*
`--lcores` token, so the user's raw value would be discarded with no
diagnostic — exactly the silent surprise the typed path was meant to
prevent. Scan `extra_args` for both `--lcores=...` (single-token)
and `--lcores` (two-token) when typed pins are non-empty, fail
fast with a clear error before any pin registration.

### Fixed — `DpdkPoller::maybe_dispatch_icmp_` guards empty callback

`set_icmp_callback` is install-once: a default-constructed
`std::function` validly claims the slot and blocks subsequent
installs (documented by the existing
`SetIcmpCallbackEmptyIsAlsoInstallOnce` test). When a real ICMP
Type 3 Code 4 message later arrived, `maybe_dispatch_icmp_` invoked
the empty function, throwing `std::bad_function_call`; the method is
`noexcept`, so the throw terminated the process. Add an
`if (!icmp_cb_)` short-circuit mirroring the same guard already
present in `IcmpRegistry::dispatch`. Reproducer test injects a real
ICMP T3C4 mbuf via the test seam after an empty install.

### Fixed — `keepalive_interval_cycles_` TSC fallback aligned with DNS

`TcpSession::keepalive_interval_cycles_()` returned
`static_cast<uint64_t>(ns)` when `TSC::to_cycles` failed (TSC not
yet calibrated), implicitly assuming 1 GHz. The DNS resolver's
matching fallback for `timeout_cycles_` / `retry_interval_cycles_`
chose 3 GHz on the same code path with the rationale "an
upper-bound frequency keeps the timer firing LATE rather than
EARLY". Keeping the keepalive at 1 GHz means a real 3 GHz host
without `TSC::init()` (typical mock setups) divided the keepalive
interval by 3× and produced probe storms; a 30 s configured
interval fired every 10 s.

Match the DNS path: the keepalive fallback now multiplies by 3.0
(3 GHz upper-bound). Production paths call `TSC::init()` at
startup and never reach the fallback. Test paths that bypass
`TSC::init()` now match the configured interval to within the
real CPU frequency rather than aggressively over-firing.

### Fixed — `UdpConfig::warnings` parity with TcpConfig (zero MAC + loopback)

`eph::dpdk::TcpConfig::warnings` has surfaced four advisory checks
since its inception (loopback src_ip, loopback dst_ip, all-zero
src_mac, all-zero dst_mac, plus self-connect and MSS) — these are the
silent-fail classes that pass `validate()` but produce a NIC frame
the switch silently drops. The matching `UdpConfig::warnings` only
flagged `hw_cksum=true`, leaving every UDP caller using designated
initialisation (UdpConfig{.src_ip=, .dst_ip=, ...}) without
src_mac/dst_mac to debug a black-hole socket from scratch.

Add the loopback-IP and zero-MAC checks to `UdpConfig::warnings` so
both transports surface the same advisories. The `<vector>` and
`<cstring>` headers were already used implicitly via spdlog/format
transitive pulls — make them explicit so the file is robust to any
future header-include reshuffle.

### Fixed — `query_rss_state` ceiling-divide RETA group count

`query_rss_state` initialises the per-group RETA mask before calling
`rte_eth_dev_rss_reta_query`, but the group count was computed via
truncating integer division — `groups = reta_size /
RTE_ETH_RETA_GROUP_SIZE`. With a non-power-of-two `reta_size`
(legal per the DPDK API; reported by some IDPF / exotic PMDs),
the tail of `reta_size` entries falls in a partial group whose
mask was therefore left at 0. DPDK then declined to populate
those entries on query, leaving `state.reta[partial_group].reta[*]`
at the value-init zeros. `queue_for_tuple` subsequently routed
every 5-tuple whose hash landed in that tail to queue 0 —
silently, with no diagnostic.

All current production NICs (Mellanox, Intel, ENA) report
`reta_size ∈ {64,128,256,512}`, so this bug is latent in practice;
the fix is forward-defensive. Use ceiling division so a partial
group still gets `mask = ~0` and the query populates every entry.
Loop bound is bounded by `reta[]`'s 8-slot array (512 entries =
DPDK upper bound), unchanged.

### Fixed — `DpdkUdpSocket::process_burst_` codec-error log parity with kernel

A decode failure on an inbound UDP datagram surfaced as a one-line
`SPDLOG_WARN("codec decode err={}")` with only the error detail —
no `src`, no `payload_len`, no count of frames already delivered
on this datagram. The kernel-side `KernelUdpSocket::poll_once_`
has logged the full `{src, payload_len, delivered_before_err}`
shape at `ERROR` since 5c44e99, with the rationale that a UDP
codec error is a market-data-loss event the operator must see
(per-packet codec, no session corruption, but the lost frames
will not retransmit). The DPDK backend silently disagreed.

`process_burst_` now tracks `delivered` inside the sink lambda and,
on `decode` failure, emits `SPDLOG_ERROR` with the same four-field
shape as the kernel. `kCodecErrors` accounting is unchanged. A
venue running both backends can now grep one query across logs
when investigating a feed gap.

### Fixed — `~DpdkUdpSocket` now clears multicast MAC filters on the NIC

`DpdkUdpSocket::join_multicast()` pushes MAC filters into the NIC's
multicast filter table, but the destructor only auto-detached from
the Poller — it never tore the filters back down. Stale entries
survived socket destruction; after enough socket churn the NIC's
finite filter table (commonly 8-16 slots on AWS ENA) was exhausted,
and operators inspecting the running NIC saw filters that no longer
mapped to any owning socket.

`~DpdkUdpSocket` now mirrors `~MulticastReceiver::leave_all_groups()`:
when `mcast_count_ > 0`, it zeroes the in-process list and best-effort
calls `apply_mcast_list_()` (return-value ignored — dtor cannot
propagate). Symmetric with the existing auto-detach contract.

### Fixed — AsyncDnsResolverT auto-detaches from Poller on destruction (UAF)

`AsyncDnsResolverT` had no destructor. The lifetime doc claimed both
destruction orders were safe ("optionally remove() before destruction;
the Poller's destructor also calls notify_detached_"), but only the
Poller-first order was actually handled. If the resolver was destroyed
while still attached (resolver-first ordering), the Poller's
`entries_` array kept a `void*` to the freed resolver — the next
`poll()` cycle dispatching a stray DNS reply would invoke the
`process_burst_fn` thunk on a dead pointer (use-after-free).

`~AsyncDnsResolverT` now calls `attached_to_->remove(this)` when still
attached, symmetric with `~DpdkTcpStream` and `~DpdkUdpSocket`. The
`attached_to_` field type changed from `void*` to
`::eph::net::dpdk::DpdkPoller<void>*` (still implicitly accepted by
the `notify_attached_` concept arg) so the dtor can call back into
the Poller. `dns.hpp` now includes `eph/net/dpdk/poller.hpp` for the
full type — no circular dependency (poller.hpp does not include dns.hpp).

### Fixed — TcpSession::operator=(&&) now matches dtor RST-on-overwrite policy

Move-assigning into a live `TcpSession` (target in `Established` /
`SynSent` / `SynReceived` / `CloseWait` with a non-null `pool_`)
silently abandoned the peer: the target's old state was overwritten
without firing the best-effort RST that `~TcpSession` would have
emitted. The peer was left half-open until its own keepalive /
read-timeout fired (minutes to hours).

Move-assign now invokes the same `should_rst_on_destroy_(state_) &&
pool_ != nullptr` policy as the destructor and calls `reset()` before
overwriting fields. The move ctor is unaffected — it constructs a
brand-new instance, so there is no prior state to wind down.

### Fixed — StreamSnapshot.tcp.peer_mss now reports the raw peer advertisement

`DpdkTcpStream::snapshot().tcp.peer_mss` previously returned
`TcpSession::effective_mss()`, which is the **clamped** value
(`min(local, peer SYN-ACK MSS)` further reduced by ICMP Frag Needed).
After an ICMP PMTU shrink, `effective_mss < peer's actual SYN-ACK
advertisement`, so the snapshot field contradicted its documented
contract ("MSS from peer SYN-ACK, 0 if not negotiated"). Operators
diagnosing path-MTU issues lost the ability to distinguish "peer
advertised X, router shrank us to Y" from "peer never advertised at
all" without an additional probe.

`TcpSession` now records the raw peer MSS at SYN-ACK time
(`TcpSession::peer_mss()`) and the snapshot reads from there, leaving
`effective_mss` to track the post-clamp/post-ICMP value. No public
DPDK API surface changed; the fix is observable only in
`StreamSnapshot::Tcp::peer_mss` semantics.

### BREAKING CHANGES — StreamSnapshot unification + enable_rss removal (2026-04-29)

Stream-level diagnostic getters and the `PlatformConfig::enable_rss`
flag have been removed in favour of a unified post-create state view
and `nb_rx_queues > 1` derivation. **Breaking** for any caller that
queried per-getter or set the flag explicitly.

| Removed                                          | Replacement                                                              |
|--------------------------------------------------|--------------------------------------------------------------------------|
| `DpdkTcpStream::tls_was_resumed()`               | `stream->snapshot().tls.was_resumed`                                     |
| `DpdkTcpStream::is_tls_send_desynced()`          | `stream->snapshot().tls.send_desynced`                                   |
| `KernelTcpStream::tls_was_resumed()`             | `stream->snapshot().tls.was_resumed`                                     |
| `Platform::is_rss_active()`                      | `platform.dispatch_mode() == RxDispatchMode::RssPartitioned`             |
| `PlatformConfig::enable_rss`                     | Field deleted; `nb_rx_queues > 1` auto-engages RSS/FlowDirector bring-up |
| `PlatformConfig::to_json` `"enable_rss"` key     | Field absent from JSON output                                            |

The previous "`enable_rss=false && nb_rx_queues > 1` → `Platform::create`
hard-fails" combination is no longer expressible. Recovery hint in the
both-RSS-paths-failed error now points at `nb_rx_queues=1` and "use a
NIC whose PMD supports rss_hash_update or rss_hash_conf_get".

`TcpSession::effective_mss()` / `peer_mss_negotiated()` (internal API,
not part of the user-facing stream surface) are **retained**;
`StreamSnapshot::Tcp` reads from them. `Platform::dispatch_mode` /
`effective_rx_queue_range` / `rss_using_probed_key` are also retained
(Platform-level diagnostics that need to be queryable before any stream
exists, and used internally by `create_and_attach` queue selection).

Migration map (mechanical):

```
- if (stream->tls_was_resumed()) { ... }
+ if (stream->snapshot().tls.was_resumed) { ... }

- if (stream->is_tls_send_desynced()) { ... }
+ if (stream->snapshot().tls.send_desynced) { ... }

- if (platform.is_rss_active()) { ... }
+ if (platform.dispatch_mode() ==
+     ::eph::net::dpdk::RxDispatchMode::RssPartitioned) { ... }

  PlatformConfig pcfg{};
  pcfg.nb_rx_queues = 4;
- pcfg.enable_rss   = true;     // line removed; nb_rx_queues > 1 suffices
```

New programmatic affordance: `snapshot().endpoint.src_port_rewritten`
exposes whether RSS reverse-pick changed the caller's pre-chosen
src_port. Replaces the `binance_latency.cpp` `warned_src_port_override`
one-shot warn pattern with a query.

### BREAKING CHANGES — StreamConfig reshape (2026-04-29, T3.19)

`DpdkTcpStream::StreamConfig` reorganized into a backend-symmetric shape
with shared `WsConfig` / `KeepaliveConfig` sub-configs and a `Dpdk`
sub-struct collecting all PMD-only knobs. Migration map:

| Old field                              | New field                          |
|----------------------------------------|------------------------------------|
| `cfg.legacy`                           | `cfg.dpdk.tcp_low_level`           |
| `cfg.pool`                             | `cfg.dpdk.pool`                    |
| `cfg.pin_to_queue`                     | `cfg.dpdk.pin_to_queue`            |
| `cfg.pool_lcore_hint`                  | `cfg.dpdk.pool_lcore_hint`         |
| `cfg.ws_path`                          | `cfg.ws.path`                      |
| `cfg.ws_host`                          | `cfg.ws.host`                      |
| `cfg.ws_extra_headers`                 | `cfg.ws.extra_headers`             |
| `cfg.ws_timeout`                       | `cfg.ws.timeout`                   |
| `cfg.ws_permessage_deflate`            | `cfg.ws.permessage_deflate`        |
| `cfg.legacy.keepalive_interval`        | `cfg.keepalive.interval`           |
| `cfg.legacy.keepalive_probes`          | `cfg.keepalive.probes`             |
| `cfg.proxy`                            | **REMOVED** — see below            |

The `legacy` rename to `tcp_low_level` is intentional: this struct is
the wire-level `TcpConfig` the PMD ingests, not a deprecated holdover.

**Keepalive** is now a public top-level knob (`cfg.keepalive`,
`KeepaliveConfig`) — `DpdkTcpStream::create` lowers it back into
`cfg.dpdk.tcp_low_level.keepalive_*` for the existing PMD machinery
(`TcpSession::tick_keepalive`). User code that previously set
`cfg.legacy.keepalive_interval / probes` should set `cfg.keepalive`
instead. The kernel backend now honours the same sub-config too.

**Proxy field removed**. The `cfg.proxy` field on the DPDK
`StreamConfig` was a "ghost" — present so user code could write
generic config-construction helpers that compiled against both
backends, but rejected at runtime with `Error::InvalidConfig`. T3.19
removes it from the DPDK `StreamConfig` entirely; misuse is now a
compile-time error pointing users at the kernel backend, which is
the only backend that supports HTTP CONNECT. Two tests in
`test_dpdk_tcp_stream.cpp` (the `ProxyConfigRejectedWithInvalidConfig`
runtime check and the `DefaultStreamConfigHasNoProxy` shape check) are
intentionally retired — the compile error is the new contract.

UdpConfig is intentionally **unchanged** in T3.19. It has no WS / TLS
/ proxy duplication with the kernel UDP config and revisiting it now
is ROI-negative; future reshape can align it if symmetry becomes
useful.

`DpdkTcpStream::create` factory order changed slightly: keepalive
validation/lowering happens BEFORE `tcp_low_level.validate()` so the
PMD sees the final lowered values. Validation strings now come from
the public `WsConfig::validate()` / `KeepaliveConfig::validate()`.

### BREAKING CHANGES — RSS bring-up reshape (2026-04-28)

`Platform::create` no longer silently degrades multi-queue configurations
to a single working queue when RSS bring-up partially fails. The
previous behaviour — collapse RETA to queue 0 + pin `dispatch_mode` to
Software — silently masked two real problems:

  1. Users requesting `nb_rx_queues=N` got 1 queue's throughput on PMDs
     that reject `rte_eth_dev_rss_hash_update` (notably ENA), with only
     an INFO-level log to indicate it.
  2. Any caller that subsequently invoked `predict_rss_queue` would use
     `kRssDefaultKey` even though the NIC was running its own internal
     default key — silently returning wrong queue indices.

New flow when `enable_rss=true && nb_rx_queues > 1`:

  * `configure_rss` succeeds → `rss_active=true`, eph's key installed
    (unchanged path).
  * `configure_rss` fails → probe via `rte_eth_dev_rss_hash_conf_get`
    after port start.
      * Probe returns a key (`key_len > 0`) → `rss_active=true`,
        `rss_using_probed_key=true`. RssPartitioned mode genuinely
        usable; `predict_rss_queue` transparently uses the probed key.
      * Probe also fails → `Platform::create` returns an error citing
        both PMD failures + a recovery hint.

Additionally, `enable_rss=false && nb_rx_queues > 1` now hard-fails with
the same recovery shape — the previous silent collapse hid this
misconfiguration too.

**Recovery for callers hitting the new hard-fail**:

  * If you wanted multi-queue parallelism: set `enable_rss=true` and
    confirm your PMD supports `rss_hash_update` or `rss_hash_conf_get`.
  * If you wanted single-queue: set `nb_rx_queues=1` explicitly.

### Added

- `Platform::rss_using_probed_key()` — diagnostic getter returning
  `true` when RSS is active and the prediction key was probed from the
  NIC rather than installed by `configure_rss`. Useful for asserting
  expected bring-up path in operational dashboards.

### Tests

- New integration test binary `test_dpdk_rss_bringup` covers the new
  configuration matrix (multi-queue probe-or-fail / multi-queue without
  RSS hard-fail / single-queue unchanged). SKIPs cleanly when NIC_B
  isn't bound to vfio-pci.
- `tests/integration/mock_dispatcher.hpp::run_mock_dispatcher` now
  self-arms `prctl(PR_SET_PDEATHSIG, SIGTERM)` so an orphaned mock
  child auto-exits when its parent test process dies abnormally.
  Fixes a class of orphan-hang infrastructure bugs observed in the
  autonomous-loop runner: a `test_dpdk_rss_fanout` mock survived 4h+
  in `sigsuspend` after its parent gtest runner died without firing
  the existing `ChildReaper` RAII, holding ports 19000-19499 and
  blocking subsequent integration suite runs with `EADDRINUSE`. The
  watchdog lives on the child side so all three call sites
  (`test_dpdk_e2e` via `dpdk_e2e_env.hpp`, `test_dpdk_rss_platform`,
  `test_dpdk_rss_fanout`) benefit without per-call wiring. Errors
  from `prctl` are deliberately swallowed — defensive fallback, the
  existing `ChildReaper` still covers the happy path.

### Fixed — RSS-pinned stream handshake silently times out (2026-04-28)

- `DpdkTcpStream::create_and_attach` (and the matching UDP path) in
  `RssPartitioned` mode resolved `pin_to_queue=N` correctly at the
  RSS-prediction layer (engineered an `src_port` whose 5-tuple hashes
  to queue N, set `target_qid=N` for the Poller-attach step) but
  forgot to align `cfg.legacy.{rx,tx}_queue_id` with `target_qid`.
  Effect: `TStream::create()` drove the SYN/SYN-ACK/ACK handshake by
  polling `cfg.legacy.rx_queue_id` (default 0 from `make_tcp_config`)
  while the SYN-ACK actually landed on queue N per the engineered
  src_port. Result: every `pin_to_queue!=0` attach silently timed out
  at 10s. Single-queue-0 deployments worked by accident (queue 0 is
  the catch-all on ENA, masking the misalignment).
- New regression `PlatformRssFanout.NStreamsDistributedAcrossQueues`
  (`tests/integration/test_dpdk_rss_fanout.cpp`) exercises this on the
  real NIC.

### Added — Multicast RSS multi-queue safety gate (2026-04-28)

- `MulticastConfig::rss_active_multi_queue` (default `false`). When the
  underlying Platform has RSS active across multiple queues,
  `MulticastReceiver::start()` now fail-fasts unless the caller
  explicitly clears this flag *and* installs a per-group FlowDirector
  rule pinning each joined group to `rx_queue_id`. Multicast UDP is in
  the project's RSS hash set (`RTE_ETH_RSS_NONFRAG_IPV4_UDP`), so
  inbound packets get hashed across queues with no caller-controllable
  steering input. Previously the receiver only polled one queue and
  silently dropped the rest. Default value preserves pre-fix behaviour
  on single-queue / non-RSS Platforms.
- `MulticastConfig::dump()` and `to_json()` now surface
  `rss_active_multi_queue` so operators see the flag value in logs and
  monitoring dashboards.

### Fixed — find_src_port_for_queue Toeplitz argument transposition (2026-04-28)

- The pre-fix helper put the local `sp` candidate in the *src_port*
  slot of the RSS hash input while the inbound SYN-ACK has it in the
  *dst_port* slot. Toeplitz is not symmetric in argument order — the
  predicted queue diverged from the queue the NIC actually picked,
  ~75% of the time on a 4-queue RETA. Helper signature now takes
  `(remote_ip, remote_port, local_ip)` explicitly and searches `sp` in
  the dst_port slot. Companion fan-out distinctness counter
  (`g_src_port_search_counter`) preserves N concurrent fan-out streams
  to the same exchange landing on distinct 5-tuples.

### Hardened — DNS pointer-chain iteration cap tightened (2026-04-28)

- `select_dns_src_port_with_state::skip_dns_name` previously bounded
  pointer-chain iteration at 128 hops. A valid DNS name has at most
  127 labels (RFC 1035 §2.3.4 — total length ≤ 253 chars, ≥ 2 chars
  per label) but realistic CDN responses ship far fewer (typically
  < 10 labels per name). 128 hops is plenty of slack for an attacker
  to keep us spinning before bailing.
- New cap is 32 iterations — still 3× the worst-case observed in
  production traces but materially shrinks the wasted-CPU
  amplification. `test_dns_adversarial.PointerChainHittingIterationLimitReturnsZero`
  uses a chain of 200, exceeds the new limit, still trips correctly.
  New explicit `kMaxIterations=32` boundary regression in
  `tests/integration/test_dpdk_rss_key_correctness.cpp` (commit
  `f5c80b5b`).

### Hardened — packet_core layout pinned via static_assert (2026-04-28)

- `PacketTemplate::build_packet` does cast-pointer arithmetic with the
  hand-written `kEtherHeaderLen` / `kIpv4HeaderLen` / `kTcpHeaderLen`
  constants while DPDK later parses the same bytes through its native
  `rte_ether_hdr` / `rte_ipv4_hdr` / `rte_tcp_hdr` views. The two views
  are now explicitly tied via `static_assert(sizeof(rte_X_hdr) == kXLen)`
  in `packet_core.hpp`. Any future DPDK header-layout change that drops
  the implicit agreement now breaks the build instead of silently
  corrupting outbound packets. Behaviour unchanged.

### Review sweep (2026-04-23 / 2026-04-24)

Post-v0.1.0 review-and-implement sweep focused on easy-of-use, simplicity,
consistency, and bugs. DPDK-environment validation deferred — no NIC
attached to the review host; every change is compile-verified via the
full test build, and non-hardware-dependent tests pass (DpdkTcpStream
32/32, DpdkUdpSocket 24/24, DpdkPoller 31/31, flow_steering 45/45,
TLS handshake 2/2, ARP 23/23, DNS 61/61 + 24/24 adversarial,
packet_core 30/30).

### Fixed — TCP keepalive treats stuck NIC as dead peer (2026-04-27)
- `TcpSession::tick_keepalive` previously incremented `keepalive_misses_`
  only when the probe successfully reached the wire. If the probe could
  not be transmitted (`rte_pktmbuf_alloc` returned null, `rte_eth_tx_burst`
  returned 0 — i.e. mempool exhausted, TX ring saturated, link bounced),
  the miss counter never advanced. Combined with the rate-limit guard
  (one probe-attempt per interval), this caused the connection to silently
  loop forever with neither liveness probes nor a Closed transition,
  blocking the application's natural reconnect path. The previous
  test-suite annotation in `Keepalive,MaxUnansweredProbesDeclareClosed`
  acknowledged this gap explicitly.
- Each `tick_keepalive` call now consumes a "miss slot" regardless of TX
  outcome — the same `keepalive_probes` threshold that closes a silent
  peer also closes a stuck NIC. Both are equally unrecoverable in place.
- New `Stats::keepalive_send_failures` counter records the cause split:
  disjoint from `keepalive_probes_sent` so each tick increments exactly
  one. Both fields are now surfaced in `Stats::dump()` and `to_json()`
  for monitoring integration.
- The send-failure path also emits a WARN log with src/dst, current miss
  count, and probe budget so operators see a stuck NIC immediately
  rather than inferring it from an unexplained drop.
- Tests: replaced the placeholder Keepalive test (the original author
  had documented why pool=nullptr couldn't drive the dead-close path)
  with a focused `Keepalive,SendFailureStillAdvancesMissCounterAndClosesConnection`
  case that asserts `keepalive_probes` failed allocs → `state_=Closed`.
  Extended `TcpStats.{Dump,ToJson}IncludesTelemetryFields` to assert the
  new fields are visible. test_tcp 64/64, test_tcp_state_machine 40/40,
  test_tcp_close_reset 14/14, test_dpdk_tcp_stream 32/32,
  test_dpdk_fault_tolerance 10/10.

### Added — single-NIC multi-process (primary+secondary)
- **Platform**: `eph::dpdk::ProcType { Primary, Secondary }` +
  `PlatformConfig::proc_type / file_prefix / rx_queue_range` fields.
  All default to single-process / primary semantics so pre-MP code
  compiles byte-for-byte.
- **Platform**: `create_primary(PlatformConfig)` and
  `create_secondary(PlatformConfig)` factories alongside the existing
  `create()`. `create_primary` force-sets `proc_type = Primary` and
  delegates to `create()`. `create_secondary` force-sets
  `proc_type = Secondary`, runs `validate_config` (which polices
  `rx_queue_range`: either the `{0,0}` full-range sentinel or a
  non-empty sub-range bounded by `nb_rx_queues`), then enforces the
  secondary-only contract — non-empty `file_prefix` + valid port
  visibility (`rte_eth_dev_is_valid_port`) — and attaches via
  `rte_mempool_lookup`, skipping `rte_eth_dev_configure /
  rx_queue_setup / tx_queue_setup / configure_rss / dev_start`
  entirely. Primary's cleanup still does `rte_eth_dev_stop/close` +
  `rte_mempool_free`; secondary's cleanup is narrowed to only clearing
  the per-process view (pollers[] + mempool pointer) and leaves the
  shared port state untouched. `effective_rx_queue_range()` is a cold
  getter consumed by `create_and_attach`.
- **Source-port partitioning**: not auto-allocated by `eph-net-dpdk` —
  callers must allocate disjoint sub-ranges per process via
  `cfg.legacy.tuple.src_port`. See `docs/dpdk-multiprocess.md`.
- **Hot path zero-cost**: `inc_<StreamMetric::*>` is already
  per-instance (`alignas(64) std::atomic<uint64_t>` +
  `memory_order_relaxed`) — no code change. `rr_counter` in both
  `DpdkTcpStream::create_and_attach` and `DpdkUdpSocket::create_and_attach`
  moved from `% nb_q` to `lo + (fetch_add % (hi - lo))`, with
  `(lo, hi)` read once from `Platform::effective_rx_queue_range()`.
  Default `{0, 0}` sentinel resolves to `(0, nb_rx_queues)` — the
  single-process path computes the same target_qid it did before,
  byte-for-byte.
- **EalConfig / build_eal_argv** (`eph/dpdk/eal.hpp`): typed assembly
  of DPDK `--proc-type`, `--file-prefix`, `-l`, `-a`, plus raw
  passthrough `extra_args`. Rejects accidental emission when
  `proc_type_set` is false (lets DPDK default to auto).
- Config `dump()` / `to_json()` extended to include the four new MP
  fields.

### Tests
- **tests/legacy/test_dpdk_multiprocess_config.cpp** — unit tests
  covering: `validate_config` rx_queue_range policing (sentinel accept,
  inverted/empty/oob reject, valid sub-range accept), `create_secondary`
  contract (empty `file_prefix` reject, inverted `rx_queue_range` reject,
  full-range sentinel accept, primary-input-does-not-short-circuit
  validation), `build_eal_argv` serialization (5 cases), `rr_counter`
  range algorithm (`% nb_q` equivalence at the `{0, 0}` sentinel,
  partitioned range bounds, single-queue constant, disjoint
  primary/secondary ranges), and `Platform::effective_rx_queue_range`
  type-level contract.
- **tests/integration/dpdk_mp_primary.cpp** +
  **tests/integration/dpdk_mp_secondary.cpp** + coordinator
  `dpdk_mp_e2e.sh` — end-to-end NIC test that brings up a primary,
  attaches a secondary via shared mempool, and verifies both see their
  owned queue/src_port ranges. Skip-cleanly (GTEST_SKIP / exit 77)
  when env vars absent, NIC not bound, hugepages low, or another DPDK
  process holds the runtime dir. Follows the project rule of retrying
  once after a 3-minute wait if DPDK is busy.

### Docs
- **docs/dpdk-multiprocess.md** (new): startup/teardown ordering,
  `PlatformConfig` MP field contract, `EalConfig` / `build_eal_argv`
  usage, 1+N partitioning table, common errors, PMD caveats (mlx5 /
  ixgbe / i40e / ena), explicit out-of-scope list, orchestrator
  invocation.
- **summary.md**: new `PlatformConfig` section listing every field
  including the MP additions; new "Multi-process factories" subsection
  covering `create` / `create_primary` / `create_secondary` contract.
- **README.md**: `Testing` section expanded from 3 bullet items into a
  full per-binary table (13 public-surface tests + `dpdk_e2e`
  integration + `tests/legacy/` coverage statement). New `Benchmarks`
  section tabulating the 8 `bench_*` targets under
  `eph-net-dpdk/benchmarks/` + RX hot-path baseline + regression guard
  script. New `Fuzzers` section cross-referencing the 4 libFuzzer
  harnesses + corpus layout + Clang-only build constraint. New
  `Scripts` section listing `dpdk-setup.sh` / `dpdk-teardown.sh` /
  `check-rx-hot-path-regression.sh`.
- **ONBOARDING.md**: "How to read the code" step 8 points at the MP
  section of `platform.hpp`. New "Running as a DPDK secondary process"
  common task with a full `create_secondary` call example and an
  enumeration of the contract rejections.

### Fixed
- **Platform**: `register_poller` rejects registering the same Poller
  pointer on two different queues. Silent misroute bug — one lcore would
  receive burst dispatches from two queues but only drain one.
- **flow_steering**: `queue_for_tuple` now matches `queue_for_hash`'s
  graceful fallback on zero-sized or non-power-of-two reta_size; a
  misbehaving PMD no longer produces an out-of-bounds RETA read / garbage
  queue id.
- **Platform**: `next_valid_pool_size(0)` now returns `1` (smallest valid
  2^k - 1 with k >= 1) instead of `0`, and `next_valid_pool_size(UINT32_MAX)`
  returns `UINT32_MAX` explicitly instead of relying on wraparound.

### Changed
- **DpdkUdpSocket**: parity fixes with DpdkTcpStream — `create_and_attach`
  now emits the same WARN/INFO breadcrumbs on `find_src_port_for_queue`
  and `predict_rss_queue` as the TCP path (previously silent on UDP).
  `mcast_macs_` array size uses `::eph::dpdk::kMaxMulticastGroups`
  directly instead of a bare `8`. `create()` logs IPs as `0x{:08x}`
  matching TCP.
- **DpdkTcpStream / DpdkUdpSocket**: `is_attached_()` now forwards to the
  public `is_attached()` instead of duplicating the `attached_to_ != nullptr`
  predicate. Single source of truth for the "attached" query across the
  two concept-required names.
- **tcp_stream / poller**: named magic constants —
  `kWsHandshakeRecvBurstRetries` (was bare `16` in two WS-handshake
  sink recv loops), `kHashCollisionLogMask` (was bare `0x3ff`).
- **arp / dns**: named the startup-resolve burst-size constants
  (`kArpResolveBurstSize`, `kDnsResolveBurstSize`) replacing four copies
  of bare `16`. Added `mbuf->nb_segs > 1` rejection in `parse_arp_reply`
  and `try_parse_dns_packet` for defense-in-depth symmetric with
  `parse_ip_header`. Use `rte_pktmbuf_data_len(mbuf)` instead of raw
  `mbuf->data_len` for accessor consistency.

### Docs
- **README / summary**: ghost `Eal(argc, argv)` constructor corrected to
  the real static factory `Eal::init(argc, argv)` returning
  `std::expected`. `PollerConfig` fields updated (`rx_queue_id`, not
  `queue_id`; no `lcore` field — caller pins the thread). `OnMessage` /
  `OnDatagram` signatures use `std::span<const uint8_t>` (+ `SocketAddr`)
  not the pre-refactor `(const uint8_t*, uint16_t)`. `Eal` correctly
  labelled move-only (was "non-copyable, non-movable"). New surface
  documented: `create_and_attach`, `connect_to`, `metric()`, ICMP
  callback setter, `pick_src_port`.
- **ONBOARDING**: removed ghost `FlowSteeringTable` reference that
  never existed. Point to the real `install_flow_rule` free-function
  + `FlowRule` RAII handle on the stream/socket. Fix test path
  (`tests/test_flow_steering.cpp`, not `tests/legacy/`).
- **packet_core**: `parse_ipv4` doc fixed — says "does NOT accept leading
  zeros" but actually tolerates them as decimal (not octal); clarified
  that this eliminates the `0177.0.0.1` loopback-bypass trick but is
  otherwise permissive.
- **multicast**: clarified `ParsedUdpPacket` / `parse_udp_packet` aliases
  point to `packet_parse.hpp` (not `net_header.hpp`, which is the umbrella).
- **summary**: Config-type layouts rewritten — `StreamConfig` /
  `UdpConfig` / `PollerConfig` now reflect the real fields (`legacy`
  nested config, `pool`, `connect_timeout`, `ws_*`, `proxy`, `reasm_capacity`,
  `pin_to_queue` / `rx_queue_id`) instead of the pre-refactor
  `remote_host` / `queue_id` / `lcore` / `max_conn` fantasy layout.
  Also noted system `libdpdk` (pkg-config) is now the canonical path;
  the vcpkg DPDK path is retired.
- **README / summary / ONBOARDING**: fixed ghost `DpdkTcpSession`
  references — the actual class is `eph::dpdk::TcpSession<ReorderSlots=64>`.
  History in release-log entries preserved unchanged.

### Fuzzer build repair
- **fuzzers**: `fuzz_arp_reply.cpp` and `fuzz_dns_reply.cpp` shims
  extended with `nb_segs` field + `rte_pktmbuf_data_len` macro so the
  round-7 scatter-reject / accessor changes in `parse_arp_reply` and
  `try_parse_dns_packet` still compile against the shim DPDK structs.
  Fuzzers are not in xmake's default build graph (require clang +
  libFuzzer), so this does not affect normal builds.

### Enriched diagnostics (rounds 15-30, 2026-04-25 / 2026-04-27)
- **Errno mirroring on cold-path failures**: every cold-path DPDK
  failure that previously returned a bare `Error::*` enum without the
  underlying `rte_errno` / `rte_strerror` text now emits an actionable
  WARN/ERROR log including `rte_errno`, `rte_strerror(rte_errno)`,
  and the surrounding context (peer endpoint, queue id, attempt
  number, hostname, gateway, target tuple, etc.). Touched call-sites:
  `rte_flow_create` / `rte_flow_destroy`, `query_rss_state` (RETA
  collapse), secondary mempool `rte_mempool_lookup`, ARP refresh,
  ARP resolve mbuf-alloc, DNS resolve mbuf-alloc, multicast MC-list
  add/remove, `UdpSender::create` `rte_eth_dev_info_get`,
  `rte_eth_link_get_nowait` link query, `TcpSession::connect`
  ERROR branches, multicast group-count rollback on join failure,
  `find_src_port_for_queue` inversion error (now includes the
  actual `[lo, hi)` range so operators see why no port satisfied
  the predicate), DNS / ARP `tx_burst` failures (now actionable
  rather than "tx_burst returned 0").
- **TLS codec error path**: the in-place TLS-decrypt latch path now
  preserves the codec's `ErrorInfo::detail` through to the caller
  instead of overwriting it with a generic transport message.
- **Constexpr-promotion**: `eph::net::parse_ipv4` is now `constexpr`
  with a `static_assert` lock so the IPv4 parser is verified at
  compile time on canonical addresses, eliminating one class of
  drift between docs and behaviour.
- **Compile-time invariants**: `static_assert` added for `ProcType`
  layout (Primary/Secondary discriminant) and EAL string contract,
  catching any accidental enum reorder or layout change at build
  time rather than at first DPDK init.
- **Static contracts**: `DpdkPoller<P>` primary-template
  `hash_collision_drops()` forwarder added (was missing — secondary
  template had it; `metric()` callers on the un-specialised type
  silently returned 0).
- **Overflow guards**: `PacketTemplate::fill_packet` now rejects
  payloads exceeding the template's reserved data region instead
  of silently UB-writing past the mbuf tail; the rejection reason
  is documented inline.
- **Centralised constants**: `kJumboMaxMss` is now the single source
  of truth for the jumbo-frame MSS upper bound, replacing two
  duplicated `9000 - 40` literals across `tcp.hpp` and `platform.hpp`.
- **Secondary-process safety**: `create_secondary` now cross-checks
  `nb_rx_queues` against the live NIC's reported queue count and
  rejects mismatches before attempting `rte_mempool_lookup`, turning
  a confusing late-stage attach failure into a clear early reject.
- **Multicast docs**: documented the intentional busy-spin in
  `multicast.hpp::rx_loop` (the only DPDK RX loop that intentionally
  does not yield, called out explicitly so it is not "fixed" by a
  well-meaning future maintainer).
- **FlowRule audit**: documented `FlowRule::remove`'s audit-trail
  contract in-source so callers know the removal is logged for
  post-mortem analysis.

### Test verification
- Rounds 1-14 verified with `xmake build -g tests` (0 errors, 0 new
  warnings beyond pre-existing transitive-include hints) and targeted
  test runs across DpdkTcpStream, DpdkUdpSocket, DpdkPoller,
  flow_steering, TLS state / handshake / desync, WS handshake timeout,
  WS sink, reasm overflow, fault tolerance, UDP multicast, ARP, DNS
  (+ adversarial), packet_core format / checksum, packet_parse
  adversarial, net_header — 476 test cases across 19 binaries, 0
  failures. DPDK-hardware-dependent runtime validation (NIC rebinding,
  e2e flows) deferred to the next real-hardware session.
- Rounds 15-30 are doc-tightening / errno-mirroring on cold paths:
  no hot-path inc_<M> / poll / rr_counter / send_batch logic touched,
  so the hot-path 0/0 cost guarantee is preserved. Each commit was
  re-verified against the same compile + targeted-test set; no
  regressions introduced.

### Known issues
- **ENA PMD cleanup SIGSEGV (AWS Graviton)**: primary's `~Platform`
  chain (`rte_eth_dev_stop` → `rte_eth_dev_close` → `rte_eal_cleanup`)
  triggers a SIGSEGV inside the ENA PMD's cleanup path on AWS Graviton
  (kernel 6.1, system libdpdk, ena 2.x). Reproduces in single-process
  mode with no MP attach involved — pre-existing PMD bug, not in
  eph-net-dpdk code. Functional and measurement layers are unaffected
  (data is flushed before cleanup; e2e tests show secondary attach +
  primary functional layer both PASS, only the post-hold cleanup
  exits 139 = 128 + SIGSEGV). Mitigation: in CI, treat primary
  exit-code 139 as success when only cleanup-time crashes are
  observed; or call `_exit(0)` at the end of `main()` to bypass the
  C++ destructor chain when the primary's exit code feeds an
  orchestrator that interprets 139 as failure.

---

## [v0.1.0] — First formal release (2026-04-23)

First version-tagged snapshot of `eph-net-dpdk`. Consolidates all
prior `[Unreleased]` accumulations (2026-04-10 through 2026-04-23)
into a single tagged release. No repo history predates `v0.1.0` in
a released form — every sub-section below was already merged to
`main` but had not yet been attached to a SemVer tag.

### Included work (by theme, newest first — detailed per-section entries follow)

- **`lucky-giggling-kahan` review closeout (2026-04-23)** — 9/11 Tier 1-3
  items + 5 TD closures (TD-1 split IP/L4 cksum counter, TD-2 strict
  mode, TD-3 TCP RX cksum wire-up, TD-5 TCP drop-cause metrics, TD-6
  precise NONE-vs-BAD mask). See
  `.artifacts/decision-20260423-045825.md` and
  `.artifacts/decision-20260423-061527.md` for the full decision chain.
- **Tier 1/2 feature adds (2026-04-23)** — UDP RX cksum validation,
  reorder-overflow e2e regression, UDP drop-cause metrics,
  keepalive-exhaustion e2e, ICMP + UDP libFuzzer harnesses, RX
  hot-path microbench baseline.
- **Tier 3 docs sweep (2026-04-23)** — `docs/dpdk-tcp-implementation.md`,
  `docs/dpdk-udp-design.md`, `README.md` thread-model diagram.
- **Polish (2026-04-23)** — `DpdkPoller::remove` enum cleanup,
  observability-guide metric table expansion, perf regression guard
  script.
- **Production-hardening sweeps (2026-04-22, two rounds, 30+ commits)**
  — TLS partial-send desync latch, mbuf-lifecycle hardening,
  keepalive reset precision, etc.
- **RSS / 5-tuple routing (2026-04-16)** — DpdkPoller protocol-aware
  5-tuple dispatch; src_port allocator.
- **Design-doc cleanups (2026-04-14)** — removed dead
  `StreamConfig::reconnect` field.
- **Phase 9 recovery (2026-04-10)** — WS handshake fields on DPDK
  StreamConfig; DPDK-side HTTP CONNECT proxy rejection.

### Test / bench state at tag

- All 36 DPDK test targets + 3 kernel / metrics = 39/39 targets green
- 934 total test cases
- RX hot-path parser microbench baseline archived at
  `.artifacts/bench-rx-hot-path-20260423.txt`; regression guard
  (`eph-net-dpdk/scripts/check-rx-hot-path-regression.sh`) reports
  0/23 regressions at the tag commit.

### Known unclosed TD

- **TD-4**: NIC_B wire-level reorder via `tc qdisc netem` — environment-
  gated (needs host kernel mutation + physical NIC on non-shared host).
- **Review Tier 2 #4**: multicast live-NIC integration test — environment-
  gated (EC2 ENA multicast + VPC subnet filtering).
- **Review Tier 3 #11**: TLS record-by-record encrypt API surgery —
  signal-gated (current latch + reconnect semantic has no production
  trigger observed).

---

## [v0.1.0] — UDP RX checksum offload validation (2026-04-23)

Closes Tier 1 #1 from the `lucky-giggling-kahan` review: `DpdkUdpSocket`'s
RX hot path never read `mbuf->ol_flags`, and `Platform::configure_port`
never requested RX checksum offload in the first place. L2/L3 transmission
errors (optical bit flips, faulty switches) produced corrupted UDP
datagrams that silently reached application codecs.

### Fixed
- `DpdkUdpSocket::process_burst_` now drops mbufs flagged with
  `RTE_MBUF_F_RX_IP_CKSUM_BAD | RTE_MBUF_F_RX_L4_CKSUM_BAD`
  before parse / codec dispatch. The branch is `[[unlikely]]`-marked;
  when RX offload is off (the default) the NIC never sets BAD and the
  branch stays out of the I-cache. UNKNOWN / NONE / GOOD are accepted
  (best-effort — HFT NICs on tunnel / VLAN paths emit UNKNOWN
  legitimately, and strict-mode drops would kill healthy traffic).

### Added
- `PlatformConfig::enable_rx_checksum_offload` (default `false`,
  opt-in). When true, `configure_port()` requests
  `RTE_ETH_RX_OFFLOAD_IPV4_CKSUM | RTE_ETH_RX_OFFLOAD_UDP_CKSUM` from
  the NIC, intersected with `dev_info.rx_offload_capa`. If the NIC
  lacks a flag, WARN-logged once and the supported subset is still
  requested — never abort (worst-case equivalence with opt-in off).
- `StreamMetric::kRxBadChecksum` / `net.stream.rx.bad_checksum`:
  single counter for IP-BAD or L4-BAD drops. Kernel backends and
  DpdkTcpStream emit 0 (see TD-3 below).

### Notes
- No software checksum fallback by design. NIC capability is an
  infrastructure-level decision; a per-packet `rte_ipv4_udptcp_cksum()`
  call would violate HFT budget.
- Default path byte-for-byte unchanged (`rxmode.offloads = 0`, no
  ol_flags read) — existing tests unaffected.

### Related / follow-up (technical debt ledger)

- **TD-1** — `kRxBadChecksum` merges IP + L4. If ops ever need to
  distinguish L3 vs L4 failures, split into `kRxIpChecksumBad` +
  `kRxL4ChecksumBad`; the hot-path branch already tests each bit,
  so the change is purely a counter fan-out.
- **TD-2** — Strict mode (drop on UNKNOWN / NONE too) not implemented.
  Trigger: operator explicitly requests strict semantics.
- **TD-3** — Symmetric gap in `DpdkTcpStream::process_burst_`. TCP's
  session layer (RFC 5961 RST guard, seqnum windowing) incidentally
  blocks most bad-checksum packets, but this is accidental coverage,
  not systematic. Follow-up: an independent `/pax --fix` to wire the
  same RX offload + metric path through TcpStream.

## [v0.1.0] — Polish (2026-04-23)

Low-risk cleanup after the TD ledger closeout.

### Changed
- `DpdkPoller::remove()` returns `Error::NotFound` (was
  `Error::InvalidConfig`) when the object was never registered or was
  already removed. Nullptr still returns `Error::InvalidConfig`. The
  previously-deferred public-enum change (CHANGELOG line ~641) —
  landed now because no caller inspects the specific error code (all
  5 call sites use `(void)poller->remove(...)`), so behavioral risk
  is zero.
- `eph::core::Error` enum gains `NotFound` (appended at end for ABI
  stability). `error_name()` returns `"NOT_FOUND"`.

### Added
- `eph-net-dpdk/scripts/check-rx-hot-path-regression.sh`: wraps the
  RX hot-path bench (`bench_rx_hot_path`, Tier 2 #7 baseline at
  `.artifacts/bench-rx-hot-path-20260423.txt`) and compares each
  bench against baseline with a configurable threshold (default 5%).
  Exit code 1 on any regression; suitable for pre-PR gate or
  local-dev canary. Production-hygiene: idempotent, no host kernel
  mutation, dry-run-safe.

### Docs
- `docs/observability-guide.md` metric table extended from 6 → 21
  entries (the 6 originally listed + 15 added by this review round).
  Documents the TD-1 aggregate/split invariant and TD-2 strict mode
  flag matrix.

### Tests
- `DpdkPoller.RemoveNonRegisteredReturnsNotFound` (renamed from
  `RemoveNonRegisteredFails`) — asserts the new enum.
- `DpdkPoller.RemoveNullptrReturnsInvalidConfig` — pins the distinct
  "programming error" path that keeps `InvalidConfig`.

## [v0.1.0] — Precise NONE-vs-BAD cksum test (TD-6) (2026-04-23)

Closes TD-6 (flagged in the TD-2 CHANGELOG note) — the non-strict
hot-path drop test was `(olf & BAD_bit) != 0`, which also matched
`CKSUM_NONE` (DPDK encodes NONE as `BAD_bit | GOOD_bit`). Replaced
with `(olf & MASK) == BAD` precise equality so non-strict drops
exactly BAD — NONE now passes through.

### Fixed
- `DpdkUdpSocket::process_burst_` + `DpdkTcpStream::process_burst_`
  non-strict drop condition changed from bit-test to mask-equality
  per layer:
    ip_bad = (olf & IP_CKSUM_MASK) == IP_CKSUM_BAD
    l4_bad = (olf & L4_CKSUM_MASK) == L4_CKSUM_BAD
  Strict mode unchanged (still `!= GOOD`). Behavior delta: NONE
  packets (e.g. RFC 768 zero-checksum UDP datagrams) are now
  accepted in non-strict mode instead of being silently attributed
  to `kRxIpChecksumBad` / `kRxL4ChecksumBad`.

### Tests
New cases:
- `DpdkUdpSocketChecksum.NonStrictAcceptsNone` /
  `StrictModeDropsNone`
- `DpdkTcpStreamReorderOverflowE2E.NonStrictAcceptsNone` /
  `StrictModeDropsNone`

### Notes
- Non-strict counter semantics are now **more precise**: readings of
  `kRxIpChecksumBad` / `kRxL4ChecksumBad` pre-TD-6 over-counted by
  the NONE traffic volume. HFT colo impact nil (UDP always cksum'd,
  TCP cksum mandatory), so no production metrics are invalidated.
- The aggregate invariant
  `kRxBadChecksum == kRxIpChecksumBad + kRxL4ChecksumBad`
  is unchanged.

### Technical debt ledger after this fix
- **TD-6**: closed.
- **TD-4** (tc-netem wire-level reorder): unchanged.

## [v0.1.0] — Strict RX checksum mode (TD-2) (2026-04-23)

Closes TD-2 from the `lucky-giggling-kahan` review. Opt-in flag widens
the RX checksum drop condition from "BAD bit set" to "CKSUM_MASK !=
CKSUM_GOOD", so UNKNOWN / NONE packets are also dropped. Default
off — current best-effort semantic preserved byte-for-byte.

### Added
- `PlatformConfig::enable_strict_rx_checksum` (default false). Gated
  by `enable_rx_checksum_offload` — strict without offload has no
  effect and emits a warning (every packet would be UNKNOWN, and
  strict would drop them all → a stream that never delivers).
- `Platform::strict_rx_checksum()` getter returns the effective flag
  (logical AND of strict + offload flags), so callers don't need to
  inspect both.
- `DpdkUdpSocket::set_strict_rx_checksum_(bool)` and
  `DpdkTcpStream::set_strict_rx_checksum_(bool)` — test-facing
  injection hooks. `create_and_attach` calls the setter from
  `platform.strict_rx_checksum()` during attach.

### Changed
- `DpdkUdpSocket::process_burst_` + `DpdkTcpStream::process_burst_`
  hot-path cksum gate now branches on a stack-local `const bool strict`:
    strict == false (default): `(olf & BAD_bit) != 0` per layer (unchanged).
    strict == true: `(olf & CKSUM_MASK) != CKSUM_GOOD` per layer.
  Drop attribution still routes into split counters kRxIpChecksumBad /
  kRxL4ChecksumBad (TD-1). Strict-mode UNKNOWN packets bump BOTH
  counters (UNKNOWN is !=GOOD for both layers); aggregate
  kRxBadChecksum reads the sum.

### Tests
New cases:
- `DpdkUdpSocketChecksum.StrictModeDropsUnknown` / `StrictModeAcceptsGood`.
- `DpdkTcpStreamReorderOverflowE2E.StrictModeDropsUnknown`
  / `StrictModeAcceptsGood` — the latter uses a forward-gapped seq to
  confirm the accept path reaches `sess_.process_rx` (`out_of_order++`).

All existing cksum tests (default strict=false path) unchanged.
39/39 target regression green.

### Notes
- Known footgun (noted in enum docs, NOT fixed by this TD): under
  non-strict mode, the current `(olf & BAD_bit) != 0` test also
  matches `*_CKSUM_NONE` (which is encoded as `BAD_bit | GOOD_bit` in
  DPDK). So non-strict mode also drops NONE, attributing it to the
  BAD counter. For UDP zero-checksum datagrams (RFC 768 legal) this
  can be a false-positive. A precise fix would use `(olf & MASK) ==
  BAD_value` comparison — recorded as TD-6 for a future cleanup;
  production impact is nil on HFT colo paths where UDP always
  carries a non-zero checksum.

### Technical debt ledger after this fix
- **TD-2**: closed.
- **TD-4** (tc-netem wire-level reorder): unchanged.
- **TD-6** (new): non-strict NONE-vs-BAD mask precision. See "Notes".

## [v0.1.0] — Split RX checksum counter (TD-1) (2026-04-23)

Closes TD-1 from the `lucky-giggling-kahan` review. The single
`kRxBadChecksum` counter is replaced by two disjoint sub-counters
exposing L3 vs L4 failure source, with the aggregate preserved as a
read-on-demand sum for backward compatibility.

### Added
- `StreamMetric::kRxIpChecksumBad` / `net.stream.rx.ip_checksum_bad`:
  NIC flagged `RTE_MBUF_F_RX_IP_CKSUM_BAD`. Typically indicates switch
  misbehavior mid-flight or L2/L3-header bit flip. Ops response:
  check the switch path and optical modules.
- `StreamMetric::kRxL4ChecksumBad` / `net.stream.rx.l4_checksum_bad`:
  NIC flagged `RTE_MBUF_F_RX_L4_CKSUM_BAD`. Typically indicates
  payload-region bit flip or a mid-path NAT that rewrote L3 addrs
  without fixing the L4 pseudo-header. Ops response: check for
  rogue middleboxes / bit-error signals on the upstream link.

### Changed
- `StreamMetric::kRxBadChecksum`: retained as the deprecated-in-place
  aggregate, now computed on-demand as `kRxIpChecksumBad +
  kRxL4ChecksumBad` at read time (via `metric()`). No atomic storage
  for it; existing publish_metrics / dashboards read the sum unchanged.
  Invariant holds across backends: the aggregate equals the sum of
  the two split counters.
- `DpdkUdpSocket::process_burst_` + `DpdkTcpStream::process_burst_`:
  hot-path BAD-cksum branch now tests IP and L4 bits separately and
  increments each matching sub-counter. A single mbuf with both
  bits set bumps BOTH sub-counters (one independent failure per
  layer) — this is the intended semantic under the new invariant.

### Tests
- `DpdkUdpSocketChecksum.DropsOnBothBadFlagsBumpsBothSubCounters`
  (renamed from `...CountsOnce`) — pins the new "dual-bit →
  aggregate=2" invariant.
- `DpdkTcpStreamReorderOverflowE2E.BothBadFlagsBumpBothSubCounters`
  (renamed from `BothBadFlagsCountOnce`) — TCP-side mirror.
- Existing single-bit tests (`DropsOnL4ChecksumBad`,
  `DropsOnIpChecksumBad`, `BadL4CksumIsDroppedBeforeProcessRx`,
  `BadIpCksumIsDroppedBeforeProcessRx`) extended to assert the
  sub-counter specificity (only the matching layer bumps).

### Notes
- Behavior change for callers reading `kRxBadChecksum`: on dual-bit
  mbufs the value is 2 not 1. This matches the new invariant and is
  arguably more informative (two layers each reported a failure).
  Single-bit bumps are unchanged (1 each).
- Hot path cost: same one `[[unlikely]]` outer branch; inside it
  two masked tests instead of one. Steady-state (opt-in off or
  NIC reports GOOD) touches nothing — zero delta.
- Kernel backends continue to emit 0 for all three (aggregate and
  both sub-counters).

### Technical debt ledger after this fix
- **TD-1**: closed.
- **TD-2** (strict UNKNOWN drop mode) / **TD-4** (tc-netem wire-level
  reorder): unchanged.

## [v0.1.0] — DpdkTcpStream drop-cause metrics (TD-5) (2026-04-23)

Closes TD-5 from the `lucky-giggling-kahan` review. TCP RX side now
attributes rejected packets to three disjoint counters, symmetric to
the UDP-side Tier 2 #3 metrics. The "medium" scope chosen: gate at the
top TCP-session drop sites (parse fail + 4-tuple mismatch + duplicate
segment), NOT per-branch inside every session internal free.

### Added
- `StreamMetric::kTcpDupSegments` / `net.stream.tcp.dup_segments`:
  duplicate / past-window data segments — peer re-delivered bytes the
  receiver already ACKed. Distinct from `kTcpOutOfOrderSegments`
  (forward gap) and from the reorder-overflow "genuine loss" branch.
  Low non-zero is expected on lossy paths; a sustained rise indicates
  the peer is retransmitting a lot or a delayed-ACK path is
  misconfigured.
- `TcpSession::Stats::packets_dropped`: segments that failed
  L2+L3+L4 parsing (non-IPv4 ethertype, truncated frame, bad IHL,
  non-TCP protocol, bad TCP data offset) or matched an unrelated
  4-tuple that the Poller routed here by mistake. Exposed via the
  cross-backend `StreamMetric::kPacketsDropped` (same enum as UDP).
- `TcpSession::Stats::fragment_rejected`: segments whose underlying
  mbuf is an IPv4 fragment (MF=1 or non-zero offset), detected via
  `is_ip_fragment` peek. Exposed via cross-backend
  `StreamMetric::kFragmentRejected` (same enum as UDP).

### Changed
- `DpdkTcpStream::metric()` now returns the three new session stats
  for `kPacketsDropped` / `kFragmentRejected` / `kTcpDupSegments`.
  UDP still uses its atomic `inc_<M>()` counters_ array — the switch
  in `metric()` overrides TCP's read to the session stats pull.
- `TcpSession::Stats::dump()` / `to_json()` / `operator-` extended
  to cover the three new fields.
- `TcpSession::process_rx` instrumented at the two drop sites:
  non-match (line ~1115) and duplicate (line ~1234). Both remain
  single-instruction increments on plain `uint64_t` — matches the
  existing single-lcore non-thread-safe stats contract.

### Tests
New cases in `test_dpdk_tcp_stream.cpp` `DpdkTcpStreamReorderOverflowE2E`:
- `NonIpv4PacketBumpsPacketsDropped` — ARP ethertype mbuf → packets_dropped++.
- `FragmentBumpsFragmentRejected` — MF=1 mbuf → fragment_rejected++,
  disambiguation via `is_ip_fragment` verified.
- `DuplicateSegmentBumpsDupSegments` — seq < rcv_nxt → dup_segments++.
- `TcpDupSegmentsMetricNameWired` — pins enum ↔ name-table slot.

### Notes
- Counter semantics **disjoint** per backend: a single mbuf triggers at
  most one of `{kRxBadChecksum, kFragmentRejected, kPacketsDropped,
  kTcpDupSegments, kCodecErrors}`.
- Default path (pre-opt-in cksum offload) is byte-for-byte unchanged
  for every counter that wasn't already being bumped before this
  commit. The three new stats fields start at 0 and only advance in
  the documented conditions.

### Technical debt ledger after this fix
- **TD-5**: closed.
- **TD-1** / **TD-2** / **TD-4**: unchanged.

## [v0.1.0] — DpdkTcpStream RX checksum parity (TD-3) (2026-04-23)

Closes TD-3 recorded by the UDP-side fix commit (d22a093) — the
symmetric RX checksum gate is now wired through DpdkTcpStream,
bringing TCP to parity with UDP. Same opt-in switch
(`PlatformConfig::enable_rx_checksum_offload`), same
`StreamMetric::kRxBadChecksum` counter, same best-effort UNKNOWN
accept policy.

### Fixed
- `DpdkTcpStream::process_burst_` now drops mbufs flagged with
  `RTE_MBUF_F_RX_IP_CKSUM_BAD | RTE_MBUF_F_RX_L4_CKSUM_BAD` before
  any other gate (TLS desync, session state, reasm overflow). The
  drop runs in-place compact form so the survivor mbufs[] remain a
  contiguous burst for `sess_.process_rx`. `[[unlikely]]` + default
  opt-in off keeps the steady-state branch out of the I-cache.
- `Platform::configure_port` now also requests
  `RTE_ETH_RX_OFFLOAD_TCP_CKSUM` when
  `enable_rx_checksum_offload=true` (previously only IPv4 + UDP).
  Capability WARN message reports all three flag bits separately
  (ipv4 / udp / tcp).

### Tests
New cases in `test_dpdk_tcp_stream.cpp`
`DpdkTcpStreamReorderOverflowE2E`:
- `BadL4CksumIsDroppedBeforeProcessRx` — asserts drop + counter++
  AND that `sess_.tcp_stats().out_of_order == 0` (pins "runs before
  process_rx" invariant).
- `BadIpCksumIsDroppedBeforeProcessRx` — same for IP bit.
- `BothBadFlagsCountOnce` — dual bad bits → one increment.
- `GoodAndUnknownFlagsPassThrough` — baseline canary, packets with
  only GOOD / UNKNOWN flags reach `sess_.process_rx` untouched
  (verified via `reorder_hits` tick on a forward-gapped seq).

### Notes
- Default path (opt-in off) is byte-for-byte unchanged; all existing
  DPDK tests continue to pass without modification.
- TCP RX cksum drop is accounted in the same `kRxBadChecksum` counter
  as UDP — kernel backends and UDP-side bumps keep their zero /
  current semantics respectively.

### Technical debt ledger after this fix
- **TD-3**: closed by this commit.
- **TD-1** / **TD-2** / **TD-4** / **TD-5**: unchanged (see prior
  CHANGELOG entries).

## [v0.1.0] — Documentation sweep (2026-04-23)

Closes Tier 3 #8 / #9 / #10 from the `lucky-giggling-kahan` review.

### Added
- [`docs/dpdk-tcp-implementation.md`](../docs/dpdk-tcp-implementation.md)
  (Tier 3 #8): TCP implementation guide — state machine diagram with
  the client-only transitions actually exercised, reorder buffer
  design (linear-scan + overflow semantics + behavioral test
  pointer), delayed-ACK semantics (caller-driven tick), the
  no-retransmit contract (what's deliberately NOT done and why HFT
  colo deployments accept that trade-off), ICMP path-MTU feedback
  flow, keepalive exhaust behaviour, and the full telemetry surface.
- [`docs/dpdk-udp-design.md`](../docs/dpdk-udp-design.md)
  (Tier 3 #10): UDP design deltas vs `KernelUdpSocket` — summary
  table, fixed-peer rationale ("one socket per peer" port shape),
  no-broadcast rationale, multicast + `connect_to` interaction
  (including the A/B-feed-failover subtle case), outbound payload
  cap (65493 bytes hard), inbound drop-cause counter table, and
  a "when to pick which" decision guide.

### Changed
- `eph-net-dpdk/README.md` (Tier 3 #9): new **Thread model**
  section with an ASCII diagram showing the one-lcore-per-Poller
  rule, the control-thread-owns-setup / lcore-owns-steady-state
  boundary, and the only cross-lcore interaction (ICMP registry
  with shared_ptr + mutex). Updated the `See also` list to link
  the two new documents.

### Notes
- No code changes in this sweep; all three documents are generated
  from the current `tcp.hpp` / `tcp_stream.hpp` / `platform.hpp` /
  `icmp_registry.hpp` / `udp_socket.hpp` source of truth.

## [v0.1.0] — RX hot-path parser microbench baseline (2026-04-23)

Closes Tier 2 #7 from the `lucky-giggling-kahan` review. Phase 9 added
several defense-in-depth checks to `packet_parse.hpp` (multi-segment
reject, fragment reject, UDP length cross-check, IHL/total_length
validation) without measuring the per-packet cost of each addition.
This bench captures the current baseline so future changes can be
diff'd against a pinned reference.

### Added
- `eph-net-dpdk/benchmarks/bench_rx_hot_path.cpp` — Google Benchmark
  microbench for the RX parse path. Covers parse_ip_header,
  parse_udp_packet, parse_udp_from_ip, parse_tcp_from_ip, parse_icmp,
  and is_ip_fragment (both fragment and non-fragment paths).
- `.artifacts/bench-rx-hot-path-20260423.txt` — baseline capture with
  reproduction context (commit hash, host / toolchain / build flags /
  command) per bench skeleton §3.

### Baseline (Graviton aarch64, 2 GHz, gcc14 -O3, release)
| Target | Cost |
|---|---|
| `is_ip_fragment` (both paths) | 0.496 ns/packet |
| `parse_udp_from_ip` (layered) | 1.07 ns/packet |
| `parse_ip_header` | 1.25 ns/packet |
| `parse_tcp_from_ip` | 1.43 ns/packet |
| `parse_udp_packet` (one-shot) | 2.43 ns/packet |
| `parse_icmp` (Type 3 Code 4) | 7.29 ns/packet |

All stable (CPU-time == wall-time, no noise). Regression threshold
per bench skeleton: < 5% ignore, 5–15% explain, ≥ 15% rollback or
justify. Claims of "significant" speedup must go through
`/pax --experiment` (not part of this review round).

## [v0.1.0] — ICMP + UDP fuzz harnesses (2026-04-23)

Closes Tier 2 #6 from the `lucky-giggling-kahan` review. Extends the
libFuzzer infrastructure from 2 harnesses (DNS + ARP) to 4 by adding
coverage for `packet_parse.hpp`:

- `fuzz_icmp_reply.cpp` — drives `parse_icmp`, `parse_ip_header`,
  and `is_ip_fragment`. ICMP is the most adversarial input in the
  system (any router along the path can inject a Type 3 Code 4),
  and parse_icmp walks an embedded IP+L4 header at a caller-trusted
  offset.
- `fuzz_udp_packet.cpp` — drives `parse_udp_packet` plus the layered
  `parse_udp_from_ip` / `parse_tcp_from_ip` entries used by
  `DpdkPoller`. UDP ingress is `DpdkUdpSocket::process_burst_`'s
  first gate; the UDP-length × IP-total-length cross-check is the
  most likely off-by-one source.

Build mechanism differs from the existing `fuzz_arp_reply`: packet
parsing pulls in real DPDK mbuf / ether / ip struct definitions, so
the new harnesses build against system libdpdk headers rather than
shimming. The fuzzer never calls `rte_eal_init` — only struct
definitions and inline accessors are touched. README documents both
recipes.

Both remain intentionally outside the xmake graph (GCC 14 has no
libFuzzer); run with Clang ≥ 17 per the README workflow.

## [v0.1.0] — Keepalive exhaustion → Closed coverage (2026-04-23)

Closes Tier 2 #5 from the `lucky-giggling-kahan` review.

The existing `Keepalive.*` tests (test_tcp_state_machine.cpp) use
`pool=nullptr` so `send_keepalive_probe_` always fails at mbuf alloc
and `keepalive_misses_` never advances — the dead-connection branch
(`state_=Closed` after `keepalive_probes` consecutive misses) was
explicitly noted as untested: "pool=nullptr isn't the right way to
test the dead-close transition."

### Tests
- Added `TcpCloseResetTest.KeepaliveProbeExhaustionTransitionsToClosed`:
  drives a net_null-backed session through 4+1 ticks, asserts
  `state → Closed` on the dead-close tick and exactly
  `keepalive_probes` TX emissions (never +1 from the dead-close
  branch itself).
- Added `TcpCloseResetTest.KeepaliveWithSingleProbeExhaustsOnTwoTicks`:
  pins the `>=` comparison at tick_keepalive:1427 for the boundary
  `keepalive_probes=1` case.

Both tests reuse the existing `TcpCloseResetTest` fixture (shared
net_null Platform + mempool), so no additional EAL bring-up cost.

## [v0.1.0] — UDP drop-cause metrics (2026-04-23)

Closes Tier 2 #3 from the `lucky-giggling-kahan` review: `DpdkUdpSocket`
previously exposed only 4 RX-side metrics (kBytesRecv / kFramesDecoded /
kCodecErrors / kRxBadChecksum). Parse failures and connect_to filter
rejections were silent — operators saw no signal when upstream flow
steering misconfigured traffic, when path MTU miscalculation sent
fragments, or when codecs got packets from non-configured peers.

### Added
- `StreamMetric::kPacketsDropped` / `net.stream.rx.packets_dropped`:
  catch-all drop counter for RX packets rejected before codec dispatch
  for reasons not attributable to a more specific counter. Covers
  non-IPv4 ethertypes, truncated frames, bad IHL, multi-segment mbufs,
  UDP length mismatches, and `connect_to()` filter mismatches.
- `StreamMetric::kFragmentRejected` / `net.stream.rx.fragment_rejected`:
  dedicated counter for IP fragments (MF=1 or non-zero offset). HFT
  workloads set DF + negotiate MSS, so non-zero here is a fragment
  attack or a path-MTU misconfiguration — the operational response
  differs from the generic kPacketsDropped path.
- `eph::dpdk::net::is_ip_fragment(mbuf)` in `packet_parse.hpp`: helper
  for callers that need to distinguish "rejected because fragment"
  from "rejected because malformed" after `parse_ip_header` /
  `parse_udp_packet` returns null. Peeks only Ethernet + IP header
  enough to read `fragment_offset`; no IHL validation.

### Notes
- All four drop counters (kRxBadChecksum / kFragmentRejected /
  kPacketsDropped / kCodecErrors) are **disjoint** — a single mbuf
  increments at most one.
- Kernel backends emit 0 for both new metrics (fragments are reassembled
  or dropped by the OS stack before userspace sees them).
- DpdkTcpStream does not wire these yet; follow-up TD-5.

### Related / follow-up
- **TD-5** — Symmetric drop-cause attribution on DpdkTcpStream. TCP's
  per-session state machine handles malformed packets differently (RFC
  5961 RST guard, seqnum windowing), so the wire-up pattern differs
  from UDP. An independent `/pax --feat` when operators request it.

## [v0.1.0] — Reorder-overflow integration regression (2026-04-23)

Closes Tier 1 #2 from the `lucky-giggling-kahan` review: c90a744's
CHANGELOG explicitly deferred behavioral verification of the overflow
reset path to integration testing. `test_dpdk_tcp_stream.cpp` now hosts
`DpdkTcpStreamReorderOverflowE2E.RealReorderOverflowDrivesStreamReset`,
which drives the real `TcpSession::process_rx` overflow branch (not the
`simulate_rx_session_error_for_test_` shortcut) through
`DpdkTcpStream::process_burst_` and asserts the full chain:

- session stats: `reorder_overflows == 1`
- stream state: `Closed` (was `Established`)
- `StreamMetric::kRxSessionResets == 1`

### Tests
- Added: `DpdkTcpStreamReorderOverflowE2E.RealReorderOverflowDrivesStreamReset`
  (integration — uses DpdkTestEnv's net_null EAL + a dedicated mempool;
  no NIC_B needed).

### Related / follow-up

- **TD-4** — NIC_B wire-level reorder coverage is still a gap. Real
  wire-level reorder induction needs `tc qdisc netem reorder` + root
  + persistent host kernel state; the cost/value exceeded scope for
  this round. Add if a tc-netem test harness is justified for other
  scenarios.

## [v0.1.0] — RX-side session stall on reorder-buffer overflow (2026-04-23)

### Fixed
- `DpdkTcpStream::process_burst_` and `DpdkTcpStream::poll_once_` now
  call `sess_.reset()` when `TcpSession::process_rx` / `poll_rx`
  returns `Error::Disconnected`. Previously the `!r` branch only
  logged and returned, so on a reorder-buffer-full result (`tcp.hpp`
  process_rx:1247, which leaves `state_ = Established` and `rcv_nxt_`
  stuck) every subsequent burst re-triggered the overflow warning and
  RX callbacks silently stopped firing. Production observation: RX-only
  Binance bookTicker feeds sat idle ~10 s before the caller's external
  stall watchdog caught the silence; the stream's `state()` never
  reflected that the session had effectively died. The branch now
  mirrors the adjacent reasm-overflow branch (which already did the
  right thing), using the same inline style for locality.

### Added
- `StreamMetric::kRxSessionResets` /
  `net.stream.dpdk.rx_session_resets`: counts stream-layer-initiated
  session resets from the RX error branch. Distinct from
  `kTcpResetsReceived` (peer-initiated RST); a sustained rise signals
  upstream packet loss or NIC reordering beyond the configured
  `ReorderSlots` capacity.

### Notes
- End-to-end reproduction of the overflow path (crafted mbufs + live
  session) remains a NIC_B e2e coverage gap — the new regression test
  pins the enum ↔ name-table wiring for `kRxSessionResets` so a future
  reorder cannot silently drift the counter's string, but behavioral
  verification of the reset call itself is deferred to integration.

## [v0.1.0] — Production-hardening sweep (round 2, 2026-04-22)

A second `/pax --loop --auto` pass (2 subagent batches × 15 rounds,
16 commits total) surfaced genuine latent bugs the first sweep
missed, plus defense-in-depth parser hardening and observability
gaps. All 28 DPDK test binaries (660+ tests) pass cumulatively
against baseline `c2362fd` on GCC 14 release. Public API shape
unchanged; behavior tightenings only.

### Fixed
- `TcpConfig::operator==` dropped `keepalive_interval` /
  `keepalive_probes` from comparison — two distinct configs
  compared equal, masking config-drift in diagnostic paths.
- `eph::net::dpdk::queue_for_hash` produced OOB reads on empty
  RETA and silently-wrong queues on non-power-of-two sizes
  (`size() - 1` wraps to SIZE_MAX with an implicit AND mask).
- `DpdkTcpStream::StreamConfig` silently accepted dangerous tiny
  `reasm_capacity` values (e.g. 512 bytes) that later crashed on
  the first burst; now rejected at config time with a 4 KiB floor.
- `TcpSession::send()` and `flush_pending_ack()` cleared the
  pending delayed-ACK timer **before** calling `tx_burst`; on
  transient NIC backpressure the pending ACK was silently dropped,
  stalling peer transmission by up to ~40 ms.
- `DpdkTcpStream`'s WS Host fallback formatted the IP with bytes
  reversed (local was named `ip_be` but `dst_ip` is host order) —
  stricter servers would return 403 on the crafted Host header.
- `DpdkUdpSocket::send_to` oversize cap was `0xFFFF` (full IP
  total_length) instead of the real UDP-over-IP payload ceiling
  (`0xFFFF − kUdpAllHeadersLen` = 65 493); oversized payloads
  reached the template as `BufferFull` rather than early
  `InvalidConfig`.
- `eph::dpdk::arp::parse_arp_reply` dereferenced `mbuf->data_len`
  before the nullptr check.
- `TcpSession::reset()` burst the RST but never `++stats_.
  tx_packets`; the sole TX path missing telemetry. Reset-heavy
  workloads underreported throughput.
- `parse_ip_header` now rejects IP fragments (MF=1 or offset!=0);
  a non-first fragment lets arbitrary bytes occupy the TCP/UDP
  header slot and could impersonate any 4-tuple. HFT paths DF all
  sends anyway; this is defense-in-depth across TCP / UDP / ICMP.
- `parse_ip_header` now rejects multi-segment mbufs; all
  downstream parsers use `rte_pktmbuf_data_len` (first segment
  only), so a chained mbuf with payload extending into segment 1
  would pass bounds checks against segment-0 length then walk off
  the contiguous buffer. Standard-MTU HFT paths don't enable
  scatter; defense-in-depth for any topology that does.

### Tests
- `fuzzers/fuzz_arp_reply.cpp` + 10-seed corpus for the ARP
  parser attack surface (well-formed, empty, truncated, wrong
  ethertype, request opcode, zero / multicast sender MAC, bad
  hw_len). Out of the xmake graph per fuzzer convention — see
  `fuzzers/README.md`.
- `test_dpdk_poller`: remove-middle-of-three regression pinning
  the shift-left compaction against function-pointer-thunk
  corruption on the formerly-tail entry.
- `test_dpdk_tcp_stream`: reasm-floor exact floor-minus-one
  probe; boundary becomes self-documenting.
- `test_flow_steering`: 3 probes covering empty / non-power-of-
  two RETA and regression for the UB path.
- `test_dpdk_udp_socket`: oversize send_to boundary.
- `test_packet_parse_adversarial`: 170 lines of new ICMP
  boundary coverage (truncated header, non-Frag-Needed
  codes, undersized payload) plus 6 IP-fragment adversarial
  cases plus 2 multi-segment mbuf cases.
- `test_packet_core_checksum`: RFC 1071 known-vector sanity
  probe — prior tests self-verified only.
- `test_tcp`: operator== regression covering the dropped
  keepalive fields.
- `test_tcp_close_reset`: tx_packets counter for RST path.
- `test_arp`: null-mbuf guard regression.

### Observability
- Every parse-time reject now logs via `SPDLOG_WARN` with
  actionable context (malformed field, detected value) rather
  than silent drop.

### Deferred-item resolution (follow-up pax, same day)
- **TLS partial-send desync → fail-fast latch**
  (`DpdkTcpStream<C,EnableTls=true>::send`). When `encrypt_for_send`
  encodes the full payload, the TLS write sequence counter advances
  by the whole payload's record count. If the subsequent chunked
  `TcpSession::send` loop then returns a typed error or 0 bytes
  (BufferFull / Disconnected), the peer is missing records with
  nonces that cannot be re-emitted — the stream is permanently
  desynced. Rather than the deeper record-by-record encrypt API
  rework, a `tls_corrupt_` latch is set on any failure in the chunk
  loop (including `off==0` since encryption has already advanced
  the counter); `send`, `process_burst_`, and `poll_once_` check
  the latch and return `Error::Disconnected` + actionable detail,
  forcing the caller's reconnect policy to rebuild the session.
  New `StreamMetric::kTlsSendDesyncs` counter; new public
  `is_tls_send_desynced()` diagnostic; test-only hooks under
  `EPH_DPDK_TCP_STREAM_TEST_HOOKS`. 6 regression tests in
  `test_dpdk_tls_desync.cpp`.
- **DpdkUdpSocket::connect_to state machine** —
  `SamePeerCalledTwiceIsIdempotent` +
  `MismatchAfterMatchDoesNotUnlatch` cover the double-call
  latching behavior that complements the existing peer-mismatch
  negative cases.
- **DpdkPoller rebind cycle** —
  `ReaddSameTupleAfterRemoveSucceeds` +
  `ReaddSameTupleSurvivesMultipleCycles` pin the add→remove→add
  rebind on the same 5-tuple, including detach-hook ordering and
  ghost-slot drift guards.

### Deferred (still open for a future pass)
- Full record-by-record `encrypt_for_send` API surgery (eliminates
  the desync window entirely instead of latching on failure). Not
  justified while typical reconnect policies already react to
  `Error::Disconnected`.
- Additional wrapper failure combos (proxy invalid / ws handshake
  timeout / TLS cert fail) — the handshake-phase error paths are
  narrower than the partial-send one and lower priority.
- `DpdkPoller::remove` returning `Error::NotFound` instead of the
  current `Error::InvalidConfig` — public enum change, deferred
  to a batched enum-rename pass if/when other callsites accumulate.

## [v0.1.0] — Production-hardening sweep (2026-04-22)

A /pax --loop --auto review pass over the non-RSS surface produced
10+ small commits tightening correctness, observability, and test
coverage without changing the public API. No hot-path performance
impact; `test_dpdk_poller`, `test_dpdk_udp_socket`,
`test_dpdk_tcp_stream`, `test_dpdk_reasm_overflow`, and legacy
`test_arp` all pass.

### Fixed
- `DpdkPoller::lookup_by_5tuple_` now increments the
  `hash_collision_drops_` counter **once per packet** (previously
  once per colliding entry, inflating the metric by the hash fan-out).
  The WARN log for sustained collisions is emitted on the first drop
  and every 1024th thereafter — previously only the first drop ever
  was logged, leaving prolonged collisions or adversarial traffic
  invisible.
- `DpdkUdpSocket::connect_to` rejects any peer that does not match the
  configured fixed `cfg.legacy.dst_ip/dst_port`. Previously a mismatch
  set `connected_peer_` to a peer the inbound filter would never see,
  leaving the socket silently TX-only (send succeeds, reply is dropped).
- `~DpdkTcpStream` and `~DpdkUdpSocket` no longer swallow
  `Poller::remove` errors — a WARN log surfaces the detail so
  Poller/Stream lifecycle mismatches (double-remove, stale
  `attached_to_`) do not disappear in the dtor.

### Security
- `eph::dpdk::arp::parse_arp_reply` now rejects ARP replies with an
  all-zero sender MAC or a non-unicast (I/G bit set) sender MAC per
  IEEE 802.3. Both are malformed as Ethernet source addresses and an
  attacker could use them to poison an ARP cache that stores blindly.

### Changed
- `TcpSession::ReorderEntry` carries an explicit
  `static_assert(sizeof(data) >= net::kDefaultMss)` so the compile-
  time invariant tracks the runtime `memcpy` bound; a future resize
  that shrunk the buffer would be caught at build time.

### Scripts
- `scripts/dpdk-setup.sh` differentiates "module missing" from "module
  built into the kernel" when loading `vfio-pci`; built-in is a warn+
  continue, missing is an actionable error with install hints.
- `scripts/dpdk-teardown.sh` guards `fuser /dev/vfio/*` on
  `[[ -d /dev/vfio ]]` so a host without vfio-pci is reported
  correctly (rather than masquerading as "no DPDK processes").

### Tests
- `test_dpdk_poller`: two new cases assert that a failing duplicate
  `add` leaves the routing table and Pollable state unchanged (same
  pointer + same tuple variants). 23/23 pass.
- `test_dpdk_reasm_overflow`: new multi-round consume/append stress
  test verifies byte-for-byte content preservation across implicit
  compaction — catches off-by-one regressions that the existing
  single-shot tests would miss. 6/6 pass.
- `test_dpdk_udp_socket`: new `DpdkUdpSocketConnectTo` fixture with
  three cases covering matching peer, IP mismatch, and port mismatch.
  6/6 pass.
- `test_dpdk_tcp_stream`: five new cases cover the remaining
  `TcpConfig::validate()` failure branches (src_port=0, dst_port=0,
  mss=0, mss>9000, recv_window=0). 13/13 pass.
- `tests/legacy/test_arp.cpp`: two new cases cover the all-zero
  and multicast sender-MAC rejection paths. 22/22 pass.

### Docs
- `StreamConfig::reasm_capacity` comment now includes a concrete
  sizing recipe, per-workload reference values (WS bookTicker / L2
  snapshot / FIX bundle), the observability hook
  (`StreamMetric::kReasmOverflows`), and the N-streams footprint note.
- New `fuzzers/README.md` documenting the build / seed / run workflow
  for libFuzzer harnesses; `fuzz_dns_reply` include paths fixed to
  reflect the post-migration `eph-net-dpdk` layout.
- New `fuzzers/corpus/fuzz_dns_reply/` with 8 seed inputs (well-formed,
  empty, runt, header-only, count overflow, pointer loop, bad label
  length) to accelerate libFuzzer coverage discovery.

## [v0.1.0] — 5-tuple routing + client source port selection (2026-04-16)

### Changed
- **DpdkPoller routing key upgraded from 4-tuple to 5-tuple** (IP protocol
  added). `PollableEntry.proto` stores `kIpProtoTcp(6)` or `kIpProtoUdp(17)`.
  `detail::hash_tuple()` and `lookup_by_5tuple_()` now include the protocol
  field in both hash and full compare. This fixes two latent issues:
  - TCP + UDP Pollables sharing the same (src_ip, dst_ip, src_port, dst_port)
    can now coexist on one Poller (legitimate independent L4 namespaces).
  - Cross-protocol misrouting is eliminated — a stray TCP packet can no longer
    be dispatched to a same-4-tuple UDP Pollable (or vice versa), which was
    only prevented in practice by NIC flow-steering rules and broke silently
    in `--no-pci` test mode.
- `DpdkPollable` concept and `tuple_for_poller_()` signature gained a
  `uint8_t* proto` out-param. `DpdkTcpStream` fills `kIpProtoTcp`,
  `DpdkUdpSocket` fills `kIpProtoUdp`.
- `PollableEntry` sizeof grew from 48 → 56 bytes (still within one 64B
  cacheline; a `static_assert` guards this invariant).
- `pick_src_port()` intentionally stays 4-tuple (protocol-agnostic) — it is
  almost exclusively a TCP-client concern and the over-restriction is
  conservative rather than incorrect.

### Fixed
- `DpdkPoller::add` now rejects duplicate 5-tuples (was 4-tuples), not just
  duplicate object pointers. Error message and warn log updated accordingly.

### Added
- `DpdkPoller::pick_src_port(src_ip, dst_ip, dst_port, range_begin,
  range_end, preferred)` — advisory helper that returns an unused
  source port in the default Linux ephemeral range `[32768, 60999]`
  for a new TCP client connection. Random-start linear probe over the
  range spreads re-picks across all 28k ports, which is what lets this
  helper skip the 2MSL grace complexity: colliding with a
  recently-released port on the same 4-tuple has ~0.0036% probability
  per call, well below the noise floor of HFT reconnect workflows.
  Optional `preferred` parameter takes a soft-preference fast path.

  Typical usage:
  ```cpp
  auto port   = poller->pick_src_port(src_ip, dst_ip, 443).value();
  cfg.legacy.tuple.src_port = port;
  auto stream = DpdkTcpStream::create(std::move(cfg)).value();
  auto add_r  = poller->add(stream.get());  // authoritative
  ```

  `DpdkTcpStream::create` is unchanged — users still write the picked
  port into `cfg.legacy.tuple.src_port` and go through the existing
  `TcpConfig::validate()` which continues to enforce `src_port != 0`.
  Flow-director preregistration deployments that hand-pick a fixed
  source port are completely unaffected.

## [v0.1.0] — Drop dead reconnect field (2026-04-14)

### Changed — BREAKING
- Removed `StreamConfig::reconnect` (`ReconnectPolicyConfig`) and the
  corresponding `DpdkTcpStream::reconnect_policy_` member, mirroring
  the kernel backend change. Same rationale: the field was carried
  but never read; a retry loop inside `create()` cannot see the
  protocol-layer state (FIX Logon, kill switch, primary/backup) that
  real HFT recovery requires, and runs before the stream is attached
  to a `DpdkPoller` so no supervisor can observe it.

  Migration: drive the reconnect loop in caller code using a
  standalone `eph::net::ReconnectPolicy`. See
  `examples/session_reconnect.cpp` (kernel variant — the DPDK
  reconnect loop has exactly the same shape, only the stream type
  changes).

## [v0.1.0] — Phase 9 Recovery (2026-04-10)

### Added
- `StreamConfig` mirrors the new `eph-net-kernel` fields so that the
  same user-facing config struct shape drives both backends:
  - `ws_path`, `ws_extra_headers`, `ws_timeout` — active: the DPDK
    backend performs the RFC 6455 handshake over its own byte-sink
    adapter just as the kernel backend does.
  - `proxy` — **rejected**: `DpdkTcpStream::create()` returns
    `Error::InvalidConfig` with detail
    `"HTTP CONNECT proxy not supported on DPDK backend"` when `proxy`
    is non-empty. Kernel-only because the CONNECT tunnel requires a
    prior kernel TCP session that the DPDK path by design does not
    own.

## v3.3 (2026-04-10) — module introduced

`eph-net-dpdk` is the v3.3 successor to the legacy `eph-dpdk` module. Phase 4
created the new module name and the new `eph::net::dpdk::*` public surface,
wrapping the existing internal DPDK primitives.

### Added
- `eph/net/dpdk/tcp_stream.hpp` — `DpdkTcpStream<C, EnableTls>`. Wraps the
  internal `eph::dpdk::DpdkTcpSession` TCP state machine and exposes it via the
  `eph::net::Stream` concept. Satisfies the concept. TLS path uses the shared
  `eph::net::detail::TlsSession` wired through a `ByteSocket` adapter.
- `eph/net/dpdk/udp_socket.hpp` — `DpdkUdpSocket<C>`. Wraps the internal UDP
  sender + receive path, adds multicast helpers, satisfies `eph::net::Datagram`.
- `eph/net/dpdk/poller.hpp` — `DpdkPoller<P>`. Replaces the legacy
  `eph::dpdk::RxDispatcher` with a concept-driven heterogeneous poller: P2
  function-pointer type erase so one Poller drives any mix of `DpdkTcpStream`
  and `DpdkUdpSocket` instances.
- `eph/net/dpdk/eal.hpp` — `Eal` RAII wrapper around EAL init/teardown.
  Successor to the legacy `EalGuard`.
- `eph/net/dpdk/config.hpp` — `StreamConfig`, `UdpConfig`, `PollerConfig` for
  the DPDK backend.
- `eph/net/dpdk/detail/` — `MbufView` (the `PacketView` implementation with
  `writable_data()` for in-place mutation), `TlsState` (adapts
  `eph::net::detail::TlsSession` to the DPDK byte-socket-style interface), mbuf
  reassembly.

### Retained (internal detail — users don't touch)
- `eph/dpdk/` — the rich pre-v3.3 DPDK primitives: `eal.hpp`, `tcp.hpp`
  (DpdkTcpSession), `udp.hpp`, `rx_dispatcher.hpp`, `arp.hpp`, `dns.hpp`,
  `flow_steering.hpp`, `packet_template.hpp`, `packet_core.hpp`,
  `packet_parse.hpp`, `multicast.hpp`, `net_header.hpp`, `platform.hpp`. Phase 7
  moved these from `eph-dpdk/include/` to `eph-net-dpdk/include/` without
  renaming so git history is preserved and the internal wiring still works.

### Changed
- DPDK TLS path is now fully operational. Pre-Phase-7 the `DpdkTcpStream<C,true>`
  path was gated behind a BLOCKER sentinel because vcpkg's openssl and aws-lc had
  conflicting symbol tables. Phase 7 removed the `openssl/rand.h` pulls from
  DPDK TUs (replaced with `getrandom(2)` for ISN generation, DNS tx_id, WS mask
  pool) and introduced the `/tmp/gcc14-wrap/g++` compiler wrapper that reorders
  `-isystem` / `-L` flags so aws-lc always wins the symbol resolution race.
- RX path uses in-place TLS decrypt via `MbufView::writable_data()` and
  `aws-lc::EVP_AEAD_CTX_open_scatter`. No memcpy between wire ciphertext and
  codec plaintext.

### Notes
- The `DpdkPoller<>` default template parameter is `void` for heterogeneous /
  type-erased mode. Instantiating `DpdkPoller<MyStream>` produces a specialised
  homogeneous poller with slightly better codegen.
- Targets linking `eph-net-dpdk` must call `apply_dpdk_pmd_linkgroups()` in
  their `xmake.lua` — DPDK PMDs need whole-archive linking to register their
  drivers.
