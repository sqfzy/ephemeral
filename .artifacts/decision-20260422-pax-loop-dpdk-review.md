# Decision Record — `/pax --loop --auto` review + implement on `eph-net-dpdk`

**Date**: 2026-04-22
**Command**: `/pax --loop --auto review eph-net-dpdk 代码 && 实施. batch-util: 15轮. until: 永远`
**Mode**: auto (no interactive approval)
**Baseline HEAD**: `c2362fd`
**Terminal HEAD**: (post follow-up) — see `git log` tail

---

## Scope and framing

The loop was dispatched against the `eph-net-dpdk` module with explicit
permission to touch related code (eph-net / eph-core / eph-utils) when
a dpdk finding traced there. The prior day's sweep (2026-04-22 first
pass, commits in the `c2362fd..` ancestor chain) had already closed
several 2026-04-13 audit items; this pass aimed to find what the first
missed and to harden defensive-depth surfaces.

## Contract

- BATCH_UNTIL = 15 rounds per subagent
- UNTIL = "永远" (non-satisfiable by design)
- MODE = subagent (BATCH_UNTIL × expected per-round cost > 30 min →
  mandatory per `loop.md`)
- BOUNDS trigger = practical session cap at 2 batches = 30 rounds +
  follow-up pax for explicitly deferred items

## Outcomes

**Two subagent batches + one follow-up pax**, 20 commits total above
baseline. Cumulative diff: 30 files changed, ~900 lines added, ~25
deleted. Public API shape unchanged.

### High-signal genuine bugs surfaced (would have eventually bitten prod)

1. `34c2029` WS Host fallback IP byte-reversal — 403 from strict servers.
2. `aa8dc5e` `flow_steering::queue_for_hash` UB on empty RETA + misalignment
   on non-power-of-two sizes (`SIZE_MAX` wrap through AND mask).
3. `95bc9cd` + `4ffc10d` delayed-ACK timer cleared BEFORE `tx_burst`;
   transient NIC backpressure silently dropped pending ACKs → peer stall
   up to ~40 ms.
4. `c6cda1c` `TcpConfig::operator==` dropped keepalive fields — masked
   config drift in diagnostic paths.
5. `d42dac5` `reasm_capacity` accepted micro values (e.g. 512 bytes)
   that crashed on first burst at runtime; now floored at 4 KiB config-time.
6. `962a542` `parse_ip_header` accepted IP fragments — non-first fragment
   lets attacker bytes impersonate any 4-tuple.
7. `1491d58` `parse_ip_header` accepted multi-segment mbufs — bounds
   checks covered only segment 0.
8. `ff6c44f` UDP `send_to` cap off by 42 bytes (used `0xFFFF` instead
   of IP-header-reduced 65 493).
9. `bd1e7ce` (follow-up) TLS partial-send nonce desync — permanent
   peer AEAD failure if TCP send partially fails. Fail-fast latch
   chosen over deeper API surgery.

### Defense-in-depth / observability

- `4255b24` ARP parse null-guard.
- `170425f` RST path tx_packets counter.
- Every parse-time reject now logs `SPDLOG_WARN` with actionable
  detail instead of silent drop.

### Test surface growth

- `74206f4` fuzz_arp_reply harness + 10-seed corpus.
- `2c517ce` / adversarial ICMP coverage (170 lines).
- `041b417` poller remove-middle regression.
- `dde4cfe` reasm floor exact-boundary probe.
- `f6bc74b` RFC 1071 checksum known vectors.
- `91ea21e` UdpSocket connect_to state machine.
- `20b00f3` Poller rebind cycle.
- Plus TLS desync regression suite (6 cases).

## Key decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Subagent vs direct mode | Subagent | BATCH_UNTIL=15 × per-round cost > 30 min mandatory |
| Batches cap | 2 batches + 1 follow-up | Batch #2 hit L1→L2→L3 ESCALATION honest exhaustion (12/15 cold rounds). Batch #3 would have been pure空转. |
| TLS desync fix depth | Fail-fast latch, not record-by-record API rework | Typical reconnect policies already handle `Error::Disconnected`; API surgery crosses eph-net / eph-net-dpdk / eph-net-kernel and adds >10× implementation cost for marginal benefit |
| Cross-module touches | Allowed for deferred TLS fix (added `StreamMetric::kTlsSendDesyncs` in eph-net) | User's "连带涉及其它代码" scope permission |
| Commit granularity | 1 logical change per commit | Per CLAUDE.md project convention |
| Forbidden files | `CLAUDE.md`, `benchmarks/mockex/fixtures/ex_market_2p_sample.jsonl`, `.claude/` untouched | Not part of this task; user's pre-existing working state |

## What the loop discipline proved

- "Candidate pool drained" is NOT a valid UNTIL trigger under `loop.md`
  rules; ESCALATION L1→L2→L3 must be exhausted honestly before BOUNDS
  can be declared. Batch #2 did this correctly: 3 real finds via
  escalation (not from the pre-planned pool), then honestly cold
  rounds + documented `skip_pool` entries instead of self-terminating.
- `/pax --loop` surfaces bugs that `/pax --review` alone would not,
  because the loop forces honest ESCALATION into code not flagged by
  the original audit (the byte-reversed IP, the delayed-ACK drop, the
  operator== field drop were all ESCALATION finds).

## Still open (explicit follow-up candidates)

1. **Full TLS encrypt record-by-record API** — for callers whose
   reconnect policy cannot tolerate `Disconnected` churn. Scoped pax
   warranted when such a caller appears.
2. **Wrapper-layer handshake failure combos** — proxy invalid /
   WebSocket handshake timeout / TLS cert validation failure. Lower
   priority than partial-send since handshake failures surface
   immediately (caller knows) vs partial-send (silent corruption).
3. **`DpdkPoller::remove` error semantics** — `Error::NotFound`
   rather than `InvalidConfig` is cleaner; batch with other enum
   renames when the list reaches critical mass.

## Artifacts

- `eph-net-dpdk/CHANGELOG.md` — "Production-hardening sweep (round 2,
  2026-04-22)" section summarizes every commit.
- This record.
- Commit history `c2362fd..HEAD` is the authoritative diff; each
  commit message carries the why.
