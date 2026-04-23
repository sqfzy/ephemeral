# eph-net-dpdk changelog

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
