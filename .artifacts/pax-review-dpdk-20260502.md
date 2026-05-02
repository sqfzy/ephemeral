---
title: pax --loop --auto review eph-net-dpdk
date: 2026-05-02
branch: pax-review-dpdk-20260502
base: main @ a958115a
loop_window: 05:21:16 → 06:21:16 (1 h hardwall)
mode: subagent (3 batches)
---

# /pax --loop --auto review eph-net-dpdk — final report

## Summary

| Metric | Value |
|---|---|
| Total rounds | 15 (across 3 subagent batches) |
| Effective commits | 8 |
| `➡️` no-commit (clean review) rounds | 5 |
| `⚠️ / ❌` problem rounds | 1 (one item punted to skip_pool) |
| Net diff | `+121 / −25` across 6 files |
| LENS exhaustion | Bug L1 → Debt L2 → Cleanup L3 reached; L4 polish only |
| Build verification | every commit individually built green via gcc14-wrap (release) on a representative target |
| DPDK EAL runtime | not invoked — pure compile-only verification (host shared with main-repo PID 2334552 + hugepages 252/256 free) |

## Findings (chronological — one row per commit)

| # | Hash | Severity | Type | File:line | One-line |
|---|------|----------|------|-----------|----------|
| 1 | `4610dd95` | 🟡 Major | fix | `eph-net-dpdk/include/eph/dpdk/lcore_pin.hpp` | `parse_pin_spec` used `atoi` → silent `lcore_id=0` on typo; switched to `strtol` with full-token validation (matches `cli.hpp::consume_one`) |
| 2 | `a0c2e193` | 🔵 Minor | test | `eph-net-dpdk/tests/test_lcore_pin.cpp` | 4 regression tests pinning strict-integer behaviour (garbage lcore/cpu fields, trailing garbage, UINT16_MAX overflow) |
| 3 | `763d37e4` | 🔵 Minor | docs | `eph-net/include/eph/net/stream_metrics.hpp` | `kRxSessionResets` doc only listed `process_rx` error path; broadened to cover drain-timeout trigger on both kernel + DPDK backends |
| 4 | `76f77f4a` | 🟡 Major | fix | `eph-net-dpdk/include/eph/dpdk/tcp.hpp` | `TcpSession::reset()` left `last_rx_tsc_ / last_keepalive_tsc_ / keepalive_misses_` stale — could falsify next session's miss tally if telemetry inspected between reset and reconnect; cleared symmetrically |
| 5 | `ab4b0ba8` | 🟡 Major | refactor | `eph-net-dpdk/include/eph/dpdk/tcp.hpp` + `tests/legacy/test_tcp.cpp` | Removed `@deprecated TcpConfig::format_mac` — only legacy tests used it; migrated to canonical `net::format_mac` (no allocation) |
| 6 | `4b6e4aab` | 🟡 Major | fix | `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp` | `DpdkTcpStream::send` (plaintext branch) under-reported `kBytesSent` on partial-loop fail (returned `Unexpected` before incrementing) — mismatched kernel parity. TLS branch intentionally untouched (different semantic) |
| 7 | `868850ad` | 🔵 Minor | docs | `eph-net/include/eph/net/stream_metrics.hpp` | `kTlsSendDesyncs` doc said "DPDK + TLS only" but `KernelTcpStream::send` bumps it on the symmetric `tls_corrupt_` latch path — operators investigating a kernel-backend TLS incident would skip the metric. Corrected to symmetric coverage |
| 8 | `3383c7d8` | 🔵 Minor | docs | `eph-net/include/eph/net/stream_metrics.hpp` (concepts area) | `saturate_u16` doc pointed at a non-existent `truncating_saturate_u16` helper. The actual gate is `saturate_u16_clamps` two lines below; fixed dangling reference |

### By severity

- 🔴 Critical: 0
- 🟡 Major: 4 (one parser correctness, one reset symmetry, one deprecated-API removal, one metric parity)
- 🔵 Minor: 4 (one regression test pin, three doc/code drift fixes)
- 📎 Reference: 0

### By scope

- `eph-net-dpdk/`: 5 commits (lcore_pin parser + tests, tcp keepalive reset, format_mac removal, tcp_stream metric)
- `eph-net/`: 3 commits (all `stream_metrics.hpp` doc/concept fixes)
- `eph-codec` / `eph-core` / `eph-utils`: reviewed clean (no commits)

## No-commit clean rounds (5)

These were drilled and confirmed clean — no actual bug:

| Target | Verdict |
|---|---|
| `tcp_stream.hpp::process_burst_` / `drain` / `metric` | clean — ordering of session state vs metric increment is correct; mbuf refcount on early-return paths sound |
| `poller.hpp::set_icmp_callback` CAS + route lookup | clean — single-thread by design; release/acquire pairing on the registry shared_ptr is correct |
| `WsCodec` close-frame edge cases (post-`00a8a5f9` follow-up) | clean — control-frame interleaving with continuation handled correctly; no other `frag_opcode_` leak corners |
| `TokenBucket` / `KillSwitch` weakly-ordered race windows | clean — fence pairing on aarch64 is correct; no ABA on refill counters |
| `SocketAddr` / parse helpers (IPv6 zone-id, port=0) | clean — error path coverage adequate, no edge bugs |
| `WsConfig` / `KeepaliveConfig` / `ProxyConfig` review | clean — config-driven shape is consistent; no field asymmetries |

## Deferred — skip_pool (1)

| Item | Reason | Recommended next step |
|---|---|---|
| `Error::CodecNeedMoreData` / `Error::NoData` enum entries appear dead in production but are documented as user-facing return values | Removing them is a public-surface change with breakage risk on downstream codecs that pattern-match the enum exhaustively. Inside this loop's risk envelope (single-commit-per-fix, no public-surface churn), correct disposition is "defer". | Open a separate `/pax --reshape "deprecate Error::CodecNeedMoreData/NoData"` with a deprecation cycle: `[[deprecated]]` first, then removal one release later. Do **not** silently remove. |

## Touched files

```
 eph-net-dpdk/include/eph/dpdk/lcore_pin.hpp      | +38 −2
 eph-net-dpdk/include/eph/dpdk/tcp.hpp            | +9 −8
 eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp | +12 −0
 eph-net-dpdk/tests/legacy/test_tcp.cpp           | +7 −7
 eph-net-dpdk/tests/test_lcore_pin.cpp            | +40 −0
 eph-net/include/eph/net/stream_metrics.hpp       | +15 −8
 6 files, +121 −25
```

## Discipline observations (for future loops)

- **Batch 1 (rounds 1–2, 28 min, 2 commits)** — context-burned on broad scoping. Dispatch prompt asked for "broader scoping scan" in round 1; subagent took the bait and read too widely.
- **Batch 2 (rounds 3–8, 10 min, 4 commits + 2 ➡️)** — most efficient. Dispatch prompt gave **explicit drill targets A–E from batch 1 handoff**; subagent went straight to actionable items.
- **Batch 3 (rounds 9–15, 5 min, 2 commits + 5 ➡️)** — saturation phase. L1 (Bug) and L2 (Debt) buckets in `eph-net-dpdk/` were already exhausted; L3 expansion to `eph-net` / `eph-codec` / `eph-utils` confirmed cross-cutting code is in good shape.
- **Lesson**: when re-running a similar review loop, dispatch the first subagent with a **pre-scoped target list** (from a quick cheap scan run by dispatcher) rather than asking it to scope itself. Saves ~25% context per batch.
- **L4 polish path was not entered** — sanity check on remaining clock budget plus commit-quality threshold (each commit must be a defensible single-purpose change) made entering pure-polish territory net-negative.

## Branch shape

Single linear feature branch, 8 commits, each:

- ✅ Single logical change (no bundling)
- ✅ Conventional Commits format with `<type>(<scope>): <subject>`
- ✅ Body explains WHY + observable consequence (not narration of WHAT)
- ✅ Builds independently green via `xmake -P /tmp/pax-review-dpdk-20260502 build -y -j 2 <representative target>` with gcc14-wrap (cxx + ld + sh)
- ✅ No `--no-verify`, no amend, no force-push

Ready for review or fast-forward merge into `main`. No conflicts with `a958115a` base.

## Build constraints honored

- All builds via `xmake -P /tmp/pax-review-dpdk-20260502` (worktree absolute path, never falls back to main repo)
- gcc14-wrap on `--cxx` + `--ld` + `--sh` per `feedback_dpdk_linker_wrapper.md`
- `-j 2` cap to share CPU with main-repo PID 2334552 mockex build
- No DPDK EAL init / `lat` / `dpdk_e2e` invoked (hugepages + vfio-pci shared resources untouched)
- aws-lc-only TLS path preserved — no OpenSSL fallback proposed
