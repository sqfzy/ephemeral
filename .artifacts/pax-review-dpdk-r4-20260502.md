---
title: pax --loop --auto review eph-net-dpdk (round 4)
date: 2026-05-02
branch: pax-review-dpdk-r4-20260502
base: main @ 895cb360 (post-r3 + CLAUDE.md fix)
loop_window: 09:21:09 → 11:21:09 (2 h hardwall)
mode: subagent (4 batches)
predecessors:
  - .artifacts/pax-review-dpdk-20260502.md     (r1, merged)
  - .artifacts/pax-review-dpdk-r2-20260502.md  (r2, merged)
  - .artifacts/pax-review-dpdk-r3-20260502.md  (r3, merged)
---

# /pax --loop --auto review eph-net-dpdk — round 4 final report

## Summary

| Metric | Value |
|---|---|
| Total rounds | 4 (across 4 subagent batches) |
| Effective commits | 6 (3 fixes + 3 docs/CHANGELOG/README) |
| 🔴 Critical | 0 |
| 🟡 Major | 1 (mp_registry pre-CAS slot leak — broader than r3's narrow fix) |
| 🔵 Minor | 2 (reconnect overflow, FakeDatagram re-entrant inject) |
| 📎 Docs | 3 (README signature + test inventory, CHANGELOG sweeps, summary.md factory shape) |
| Net diff | `+207 / −24` across 11 files |
| Loop wall-clock | ~40 min (closed at ~10:01 vs 11:21 hardwall = ~80 min unused) |
| Saturation evidence | All 4 batches converged on saturation; final batch found 1 minor doc fix after drilling kernel-side TCP/UDP/poller — true L4 |

## Findings (chronological)

| # | Hash | Severity | Type | File | One-line |
|---|------|----------|------|------|----------|
| 1 | `750aa0a1` | 🟡 Major | fix | `eph-net-dpdk/include/eph/dpdk/detail/mp_registry.hpp` | `MpRegistryHandle::attach_secondary(already_claimed=true)` released the preclaimed slot only on the spec-disagree + lcore-overlap branches; every earlier validation failure (memzone lookup / magic / version / file_prefix / totals / self_index) returned with `claimed=1` still set, blocking subsequent retries from the same process. Scope-guard pattern with `disarm()` on success / explicit-release branches. **Broader fix than r3's narrow spec-disagree patch — covers the entire pre-CAS validation surface** |
| 2 | `1764beb6` | 🔵 Minor | fix | `eph-net/include/eph/net/reconnect_orchestrator.hpp` | `ReconnectOrchestrator::enter_backoff_` TSC-uncalibrated fallback `delay.count() * 3'000'000ULL` overflowed `uint64_t` when caller set `max_backoff = milliseconds::max()`, wrapping to a tiny `next_attempt_tsc_` and defeating backoff entirely. Saturating-mirror of `TSC::to_cycles`'s pattern |
| 3 | `22bdf966` | 🔵 Minor | fix | `eph-net/include/eph/net/test/fake_datagram.hpp` | `FakeDatagram::poll_once_` iterated `rx_queue_` by reference + post-loop cleared. User `on_datagram` callback calling `inject_datagram` reallocated the vector (UB on iterator) AND silently dropped the new entry on `clear()`. Swap-drain pattern + regression test. Tests using FakeDatagram's natural request-echo pattern were flaky |
| 4 | `ce4cdbeb` | 📎 Docs | docs | `eph-net-dpdk/README.md` + `eph-utils/README.md` | (a) eph-net-dpdk usage example called `Eal::init(argc, argv)` — that signature was renamed to `Eal::init_raw` (escape hatch) + `Eal::init(EalConfig, …)` (typed path). (b) eph-utils claimed "20 GoogleTest files" but tree has 22; missed `test_netns_compile` and `test_recorder_runtime` |
| 5 | `f4ccb0e0` | 📎 Docs | docs | `eph-net-dpdk/CHANGELOG.md` + `eph-net/CHANGELOG.md` | Both `[Unreleased]` sections last touched for post-rename audit; missing all r1-r4 defence-in-depth fixes. Added `Fixed (2026-05-01..2026-05-02)` subsections cataloguing each merged commit with before/after behavior. eph-net-dpdk also gained `Removed (2026-04-30)` for the prior `TcpConfig::format_mac` drop |
| 6 | `c43f1c48` | 📎 Docs | docs | `summary.md` | Stale `// ... same public surface as KernelTcpStream` shorthand for DpdkTcpStream — but `DpdkTcpStream::create(cfg, poller)` was removed (post-T3.19); only factory now is `create_and_attach(cfg, platform)` with queue selection / src_port allocation / FlowDirector / ICMP wiring at construction |

### By severity

- 🟡 **Major: 1** — mp_registry pre-CAS slot leak. **The third loop in a row to find an mp_registry slot-release bug** (r3 fixed spec-disagree branch only; r4 covers the broader pre-CAS validation surface). Release-symmetry pattern continues to be the dominant bug class
- 🔵 **Minor: 2** — reconnect TSC-uncalibrated overflow, FakeDatagram re-entrant inject. Both are defensive fixes against pathological config / API misuse but real bugs nonetheless
- 📎 **Docs: 3** — README/CHANGELOG/summary doc/code drift across 3 commits

### Themes

- **"Release symmetry" pattern continues to dominate** — r1 (keepalive_misses_) → r2 (effective_mss_) → r3 (mp_registry spec-disagree branch) → r4 (mp_registry **broader** pre-CAS validation). 4 loops, 4 separate but related bugs. **Strong evidence the pattern needs a structural solution** (e.g. RAII scope-guard in the project header, mandatory linter check)
- **Late-loop yield is doc/code drift** — by r4, code review is finding doc misalignments (README signatures, CHANGELOG gaps, summary.md shorthand). This is healthy: it means the bug-finding ROI has crossed the threshold where doc maintenance is the highest-value remaining work
- **Defensive bugs persist** — both Minor fixes (reconnect overflow, FakeDatagram inject) are "users would never hit this in normal flow" but real correctness bugs that surface under pathological configs / API misuse. Worth fixing for production assurance even if no current caller triggers them

## No-commit clean-review territory (this round)

Drilled and confirmed clean:

**Batch 1**:
- DpdkTcpStream::send / drain / on_poll_tick keepalive race (single-threaded poll model — no TOCTOU window in current architecture)
- DpdkUdpSocket::send burst-fill (mbuf release on partial TX correct; multi-segment chained mbuf TX handled)
- DpdkTcpStream::create_and_attach rollback-on-failure (Poller detach + ICMP unregister symmetric on failure paths)

**Batch 2**:
- aws-lc CSPRNG return-value audit: 8 sites verified (jwt nonce, DNS tx_id ×2, ephemeral_port, TCP ISN, WS mask key cache 3-retry, WS handshake nonce 3-retry, DpdkPoller pick_src_port). All return-checked. **No silent-zero-buffer paths**
- Kernel TCP send loop EINTR/EAGAIN/EWOULDBLOCK/EPIPE/ECONNRESET handling
- FakeStream / TestPoller lifetime (FakeStream emits whole rx_buf as one frame, no iterator window; TestPoller test fixture ownership symmetric with poller registration)

**Batch 3**:
- Module README sweep: eph-net (StreamMetric 25-entry count), eph-core, eph-net-kernel (13 tests), eph-codec (5 codecs / 8 tests) all match current code
- Bench fairness mockex side: all four echo scenarios + push helpers use `bench::monotonic_raw_ns()` exclusively, identical timestamp ordering across kernel-vs-DPDK clients (mockex never knows which client side is calling)
- Per-module CHANGELOG: eph-core's Error::CodecNeedMoreData/NoData removal already documented; only eph-net + eph-net-dpdk needed updates (committed)
- Non-DPDK module TODO/FIXME sweep: 30 grep hits all spurious (FIX checksum format `10=XXX`, JSON escape `\uXXXX`, test fixture API key placeholders); 1 well-documented "KNOWN LIMITATION" in `eph-utils/audit_log.hpp` re LMAX-disruptor — non-actionable, leave alone

**Batch 4**:
- r3 + r4-batch1 mp_registry fixes compose correctly (r4-batch1 scope-guard layered on top of r3's manual releases; explicit `disarm()` avoids double-release)
- KernelPoller::poll() main loop: EINTR retry, self-removal during poll, EPOLLERR/EPOLLHUP implicit delivery — clean
- KernelTcpStream connect path: EINPROGRESS handling, POLLOUT wait + SO_ERROR check, deadline monotonic, proxy CONNECT over-read refusal for TLS/WS (designed)
- KernelUdpSocket: SO_REUSEADDR/RCVBUF/SNDBUF with INT_MAX clamp, multicast helper, sendto errno classification (EMSGSIZE→InvalidConfig, ENOBUFS→BufferFull), keepalive setsockopt sequence with probes==0 pre-check

## Touched files

```
 .../include/eph/dpdk/detail/mp_registry.hpp        | +33 -8
 eph-net/include/eph/net/reconnect_orchestrator.hpp | +25 -4
 eph-net/include/eph/net/test/fake_datagram.hpp     | +14 -4
 eph-net/tests/test_fake_datagram.cpp               | +51 -0
 eph-net-dpdk/README.md                             |  +6 -3
 eph-utils/README.md                                |  +5 -3
 eph-net-dpdk/CHANGELOG.md                          | +27 -0
 eph-net/CHANGELOG.md                               | +21 -0
 summary.md                                         |  +9 -2
 11 files, +207 -24
```

## Round 1 → 2 → 3 → 4 cumulative trend

| Metric | R1 | R2 | R3 | R4 |
|---|---|---|---|---|
| Wall-clock | 51 min / 60 min budget | 42 min / 60 min budget | 50 min / 120 min budget | 40 min / 120 min budget |
| Subagent batches | 3 | 2 | 5 | 4 |
| Total rounds | 15 | 5 | 21 | 4 |
| Effective commits | 8 | 5 | 6 | 6 |
| Critical | 0 | 1 (TLS seq overflow) | 0 | 0 |
| Major | 4 | 4 | 3 | 1 (mp_registry broader) |
| Minor / Docs | 4 | 0 | 3 | 5 |
| Saturation | L3 reached | L3 reached | L3+L4 deep | L4 fully exhausted |
| **Cumulative commits** | 8 | 13 | 19 | **25** |

**Pattern across 4 rounds**:

1. **Diminishing-returns curve continues, severity profile shifts down** — R1+R2 yielded 1 Critical + 8 Major. R3+R4 yielded 0 Critical + 4 Major. This is healthy maturity: the bugs found late are smaller AND more obscure
2. **"Release symmetry" pattern is now incontrovertible** — found in r1, r2, r3, r4. The keepalive_misses_, effective_mss_, mp_registry-spec-disagree, and mp_registry-pre-CAS-validation bugs all share the structural shape: "function has multiple early-return paths, only some of them release the resource". A `scope_exit{...}` RAII guard at the top of each such function would mechanically prevent this entire class of bug. **Strongly recommend a project-wide `release_on_unwind` audit + scope-guard introduction** (separate `/pax --reshape`)
3. **Doc/code drift is the dominant late-loop finding** — R3 batch 5 surfaced CLAUDE.md drift; R4 batch 3 surfaced module README + CHANGELOG drift; R4 batch 4 surfaced summary.md drift. Each loop, the doc surface drifts further from code. **Mechanical safeguard worth investigating**: a CI check that diffs each module's README "Public API" claims against actual `pub`/`export`-equivalent symbols in code. Out of scope for this loop
4. **R4 closed at ~33% budget utilization (40/120 min)** — the lowest of the 4 rounds. Subagent #4 made the right call to close at L4 saturation rather than manufacture commits. **Lesson**: with 25 substantive commits across 4 loops, the bug-finding ROI is now genuinely below break-even — future rounds should change LENS (perf / security / API ergonomics) or scope (release readiness via `/pax --ship`)

**Cumulative across 4 rounds**: 25 substantive commits + 4 reports across the eph stack. Validates the loop architecture as a recurring quality-bar enforcement mechanism that reaches genuine saturation in 4 rounds.

## Build constraints honored

- All builds via `xmake build -P /tmp/pax-review-dpdk-r4-20260502 -j 2` (correct flag form)
- gcc14-wrap on `--cxx` + `--ld` + `--sh`
- `-j 2` cap to share CPU
- No DPDK EAL init / `lat` / `dpdk_e2e` invoked
- aws-lc-only TLS path preserved

## Disposition recommendation

- ✅ Fast-forward merge `pax-review-dpdk-r4-20260502` → `main` (clean ff, 6 commits + this report)
- 🟠 **Strongly recommend follow-up**: `/pax --reshape "introduce scope_exit RAII guard for resource-release-on-multiple-early-returns sites"` to mechanically eliminate the bug class found 4 loops in a row
- 🟡 **Recommend follow-up**: `/pax --doc "doc/code drift mechanical check"` — explore whether a CI gate can mechanically catch the kind of README/CHANGELOG/summary.md drift that r3+r4 found
- ❌ Do NOT immediately schedule R5 — saturation is comprehensive (kernel + DPDK + utils + codec + parsers + integration + bench + examples + docs all drilled clean). The next loop's value depends on (a) new code landing, (b) different LENS (perf / security / API ergonomics), or (c) different scope (release readiness)

## Discipline observation: the saturation curve

```
Loop  | Commits | Major | Critical | Wall-clock util
------|---------|-------|----------|-----------------
R1    |    8    |   4   |    0     |    85%
R2    |    5    |   4   |    1     |    70%
R3    |    6    |   3   |    0     |    42%
R4    |    6    |   1   |    0     |    33%
------+---------+-------+----------+-----------------
TOTAL |   25    |  12   |    1     |
```

R4 is the first loop where Major < 2. Combined with R4's 33% utilization, this is the strongest signal yet that the codebase has crossed a maturity threshold — at least for the LENS (Bug > Debt > Cleanup) and scope (eph-net-dpdk + cross-cutting) used across these 4 rounds.

**Strategic implication**: future loops should NOT just re-run the same LENS+scope; they should actively change the angle of attack. The bug surface for "obvious correctness errors" has been comprehensively traversed. Remaining latent issues (if any) require either (a) new code introduction creating new review surface, (b) angle-shift to performance/security/ergonomics, or (c) deeper-than-static-review tools (fuzzers / sanitizer-CI / formal verification on critical paths).
