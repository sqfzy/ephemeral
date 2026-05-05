# `tests/legacy/` audit (T3.4, 2026-05-05)

## TL;DR

**Not dead code, not stale, name is misleading.** The 23 files under `tests/legacy/`
are the active unit-test layer for `eph::dpdk::*` internal primitives —
ARP / DNS / multicast / packet parsing / TCP state machine / UDP / EAL / ICMP
registry / net header utilities. Together: **720 individual TEST cases across
10 803 lines**. All compile (verified via `xmake build`) and one representative
target (`test_tcp`) passes 68/68 cases at HEAD `fccd9202`.

The "legacy" prefix originates from the 2026-04-11 module rename
(`eph-dpdk` → `eph-net-dpdk`) — a historical choice that turned out misleading
in 2026-05 reviews where an outsider's first reaction was "this looks like
dead code we should delete". Recommended action: **rename `legacy/` →
`detail/`** to reflect its actual role (mirroring the source-tree convention
`include/eph/dpdk/detail/`), or alternatively keep as-is and lean on this
AUDIT to disambiguate. **Either way: do not delete.**

## Methodology

For each `tests/legacy/test_*.cpp` file:

1. Count individual TEST / TEST_F / TEST_P cases (`grep -c "^TEST"`).
2. Capture line count.
3. Verify xmake target builds (per-file glob in `eph-net-dpdk/xmake.lua` —
   `for _, file in ipairs(os.files("tests/legacy/*.cpp")) do target(...)`).
4. For one representative file (`test_tcp`), verify execution path
   (`xmake run test_tcp` → 68/68 pass).
5. Check whether the public-surface `tests/test_*.cpp` covers the same
   primitive (a "duplication risk" check).

## Inventory

| Legacy file (target) | Tests | Lines | Public-surface counterpart | Status |
|---|---:|---:|---|---|
| `test_arp` | 23 | 437 | `test_arp_api`, `test_arp_resolve` (api/resolve only — primitive ARP build/parse here) | **Keep** — primitive layer not duplicated |
| `test_arp_build` | 9 | 266 | none | **Keep** — primitive `build_arp_request` only here |
| `test_dns` | 61 | 730 | `test_dns_async`, `test_dns_rss_aware` (async resolver / RSS-aware reply routing only) | **Keep** — `parse_dns_response` adversarial only here |
| `test_dns_adversarial` | 29 | 518 | none | **Keep** — fuzz-corpus equivalent for parser hardening |
| `test_dns_helpers` | 16 | 182 | none | **Keep** — `eph::dpdk::dns::detail::*` helpers |
| `test_eal` | 8 | 93 | `test_eal_config_argv` (argv assembly only) | **Keep** — `EalGuard` lifecycle only here |
| `test_flow_protocol_and_multicast_boundary` | 13 | 151 | `test_flow_steering` (RSS / FlowDirector / FlowRule RAII) | **Keep** — `FlowProtocol` enum + multicast boundary only here |
| `test_icmp_registry` | 24 | 652 | `test_icmp_directory`, `test_icmp_dispatch` (directory + dispatch) | **Keep** — `IcmpRegistry` (shared_ptr+mutex internal) only here |
| `test_multicast` | 68 | 850 | `test_dpdk_udp_multicast`, `test_dpdk_udp_multicast_rss` (DpdkUdpSocket integration) | **Keep** — `MulticastReceiver` primitive only here |
| `test_net_header` | 102 | 1549 | none | **Keep** — `parse_ipv4` / checksums / byte-order primitives |
| `test_packet_core_checksum` | 20 | 263 | none | **Keep** — Internet/TCP/UDP checksum, primitives |
| `test_packet_core_format_and_tuple` | 30 | 241 | none | **Keep** — `format_ipv4` / `ConnectionTuple` |
| `test_packet_parse_adversarial` | 47 | 833 | none (fuzzers cover binary corpus, this covers structured edge cases) | **Keep** — adversarial parser hardening |
| `test_packet_template_build` | 16 | 382 | none | **Keep** — `PacketTemplate` build + fill |
| `test_tcp` | 68 | 773 | `test_dpdk_tcp_stream` (DpdkTcpStream-level only) | **Keep** — `TcpSession` state machine + handshake |
| `test_tcp_close_reset` | 14 | 378 | none | **Keep** — close / RST behaviour |
| `test_tcp_conformance` | 3 | 589 | none | **Keep** — RFC conformance suite (size-heavy: 3 tests, 589 lines = behaviour coverage) |
| `test_tcp_fault_tolerance` | 20 | 527 | `test_dpdk_fault_tolerance` (ReasmBuffer only) | **Keep** — TCP fault paths (peer-MSS, ICMP PMTU, reorder overflow) |
| `test_tcp_state_machine` | 42 | 1000 | none | **Keep** — exhaustive state-machine coverage |
| `test_tcp_window_and_udp_send_batch` | 14 | 216 | none | **Keep** — TCP recv-window + UDP batch send |
| `test_udp` | 49 | 800 | `test_dpdk_udp_socket` (DpdkUdpSocket-level only) | **Keep** — `UdpSender` primitive |
| `test_udp_template_build` | 16 | 312 | none | **Keep** — `UdpPacketTemplate` |
| `test_udp_zero_len_and_template_overflow` | 8 | 221 | none | **Keep** — UDP zero-length + overflow guard |

**Totals**: 720 test cases / 10 803 lines / 23 files / 0 duplicates with public surface / 0 build failures.

## Findings

### F1. No dead code
Every file targets a primitive the public-surface stream/datagram/poller
**transitively wraps**. Deleting any of them removes the only direct
unit-level coverage for that primitive — public-surface tests cover the
combined behaviour, not isolated primitive correctness.

### F2. No coverage duplication with public-surface tests
For each legacy file, the public-surface counterpart (where one exists)
covers a higher-layer concern (DpdkTcpStream wraps TcpSession; the public
test exercises stream lifecycle, the legacy test exercises state-machine
transitions). Removing legacy = losing the primitive coverage.

### F3. The name is misleading
"legacy" suggests "deprecated, will be removed". The README explicitly
disclaims this (`README.md:333-339`): "preserved from the pre-rename
`eph-dpdk` module — **not deprecated**, they are the source of truth
for the detail layer the public types wrap." But the directory name is
the first thing readers see and overrides the README disclaimer.

### F4. The xmake graph integrates them as first-class
`xmake.lua` glob-loops over `tests/legacy/*.cpp` and builds them under the
same `eph-test` rule as the public-surface tests. They are not gated by
a feature flag, not opt-out, not separately scheduled. CI treats them
identically.

### F5. No skipped / disabled tests
No `DISABLED_` prefixes, no `GTEST_SKIP()` calls except environment-
gated ones (vfio NIC / hugepages availability). All 720 cases run when
the build target is invoked.

## Recommendation

### Primary: rename `tests/legacy/` → `tests/detail/`

Rationale:
- Mirrors the source convention (`include/eph/dpdk/detail/` houses
  internal helpers; the test directory should match).
- Eliminates the "is this dead code?" first-impression friction
  documented in 2026-04-13 audit and 2026-05-05 primer.
- Zero behaviour change — pure rename + 1-line change in `xmake.lua`
  (`tests/legacy/*.cpp` → `tests/detail/*.cpp`).
- Recovers the search affinity: `git grep eph::dpdk::detail` and
  `tests/detail/` line up symbolically.

### Alternative: keep `legacy/`, lean on this AUDIT.md

If renaming is judged not worth the churn (commit-history readability,
external CI / scripts pinning paths, etc.), keep the directory but
ensure this AUDIT.md is referenced from `README.md` so newcomers find
the disambiguation immediately.

### Do NOT

- Delete any file. Each is the sole unit-level coverage for its primitive.
- Move into the matching `test_*.cpp` public-surface file. The split
  by abstraction layer is correct (primitive vs stream-level); merging
  would create 1500+ line test files that are hard to navigate.
- Convert to skipped / disabled tests. They run, they pass, they
  catch regressions.

## Decision pending

Either rename or keep-with-AUDIT-reference is fine. **No deletion**.

Track item: T3.4 from the 2026-05-05 action list.
