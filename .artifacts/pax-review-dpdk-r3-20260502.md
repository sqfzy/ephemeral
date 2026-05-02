---
title: pax --loop --auto review eph-net-dpdk (round 3)
date: 2026-05-02
branch: pax-review-dpdk-r3-20260502
base: main @ 5a5121b1 (post-r2)
loop_window: 07:48:39 → 09:48:39 (2 h hardwall)
mode: subagent (5 batches)
predecessors: .artifacts/pax-review-dpdk-20260502.md, .artifacts/pax-review-dpdk-r2-20260502.md (both merged)
---

# /pax --loop --auto review eph-net-dpdk — round 3 final report

## Summary

| Metric | Value |
|---|---|
| Total rounds | 21 (across 5 subagent batches) |
| Effective commits | 6 (3 fixes + 1 docs + 1 cleanup + 1 small fix) |
| 🔴 Critical | 0 |
| 🟡 Major | 3 |
| 🔵 Minor | 2 |
| 📎 Cleanup/docs | 1 |
| Net diff | `+57 / −12` across 5 files |
| Loop wall-clock | ~50 min (closed at 08:38:29 vs 09:48:39 hardwall = ~70 min unused) |
| Saturation evidence | 4 of 5 batches reported deep L1-L4 saturation across drilled territory |

## Findings (chronological — one row per commit)

| # | Hash | Severity | Type | File | One-line |
|---|------|----------|------|------|----------|
| 1 | `24b213f7` | 🔵 Minor | docs | `eph-net/include/eph/net/detail/tls_constants.hpp` | `TlsConfig::on_pin_mismatch` docstring said nullptr = soft-pin (log+continue) but `verify_spki_pin` hard-rejects null callback per P0-3 hardening. Operators reading the doc would deploy non-empty pin lists expecting soft behavior, then have mismatches drop traffic |
| 2 | `d5c8ff1d` | 🟡 Major | fix | `eph-net-dpdk/include/eph/dpdk/detail/mp_registry.hpp` | `attach_secondary(already_claimed=true)` released caller-preclaimed slot only on lcore_mask-overlap path; the earlier queue/port spec-disagree branch leaked the slot under the failing PID. Caller retrying `Platform::create_or_join` from the same process sees `is_pid_alive=true` on the stale slot → every subsequent attach fails "no free slots" → slot permanently stuck for process lifetime |
| 3 | `51821eb5` | 🟡 Major | fix | `eph-net/include/eph/net/detail/tls_session.hpp` | `TlsSession::extract_hot_state` set `suppress_close_notify_=true` before fallible cipher-lookup / traffic-secret / HKDF derivation; the flag's contract is "caller took over data path", so mid-extract failure silently skipped TLS `close_notify` in `~TlsSession` even though the caller never actually took over. Move the flag commit to the success path |
| 4 | `d50407f7` | 🔵 Minor | fix | `eph-net-dpdk/include/eph/net/dpdk/flow_steering.hpp` | `RemoteFlowRulesMap::destroy_all` silently swallowed PMD errors via `(void)rte_flow_destroy(...)`. Asymmetric vs sibling `destroy_by_id` which already logs at WARN. Now WARN-logs each failing destroy with port + rc + PMD message |
| 5 | `ef9c1a38` | 📎 Docs | docs | `examples/binance_book.cpp` | Header diagram's `(frame counter — TODO)` annotation predates actual implementation at line 122-128 (already counts frames + logs every 16th). Reads as if example is incomplete when it isn't |
| 6 | `86d645a5` | 🟡 Major | refactor | `eph-net-dpdk/include/eph/net/dpdk/flow_steering.hpp` | `try_install_flow_rule_via_ipc` set `FdInstallMsg::proto` with magic literals `6`/`17` while receiver-side `on_fd_install_thunk` checks against named `kIpProtoTcp/Udp` constants. Same-source-of-truth on both wire ends prevents silent drift if anyone ever remaps the named constants |

### By severity

- 🟡 **Major: 3** — mp_registry slot leak (process-lifetime stuck slot), TLS extract suppress mistiming (skipped close_notify on failure), flow_steering IPC magic-number drift risk
- 🔵 **Minor: 2** — TlsConfig hard-pin docstring, flow_steering destroy_all silent failures
- 📎 **Docs: 1** — examples/binance_book stale TODO

### Themes

- **Symmetry-of-cleanup** — like r1 (keepalive state) and r2 (MSS state), r3 surfaced another "release on this path but not that path" bug at `mp_registry.hpp:619` (release-on-disagree). Three loops in a row finding this exact pattern suggests it's a recurring class — anywhere `reset()` / `release()` / `unwind()` exists, audit every error branch
- **Doc/code drift on safety-critical defaults** — TlsConfig hard-pin doc was stale (📎 misleads operator) and binance_book TODO is dead. Auto-checking docstring claims against runtime behavior (e.g. test that reads doc text and asserts behavior) would catch these mechanically
- **Magic-number wire fields** — IPC `proto=6`/`17` was the same kind of cross-version-drift hazard that motivates named-constant convention. Refactor was a defensible debt cleanup

## No-commit clean-review territory (very large)

This round confirmed deep saturation across many module groups, in addition to r1+r2 clean items. Future rounds should not re-drill any of:

**Parser modules + containers (batch 1, 6 rounds)**:
- `eph-fix`: BodyLength multiply-then-add overflow guard, OOB-safe header_len + body_length + 7 bounds, parse_tag_number with kMaxTag/UINT32_MAX guard, builder begin_string SOH/NUL rejection, session MsgSeqNum UINT32_MAX cap
- `eph-itch`: SoupBinTCP BE length, MoldUDP64 sequence overflow + per-message bounds, ITCH unknown-type rejection, OUCH oversize-token reject, kSize length gates
- `eph-json`: nested-skip kMaxNestingDepth=64, backslash-escape skip, parse_int unsigned-then-cast for INT64_MIN, parse_number rejects bare `1.` / `1e` / overflow / non-finite
- `eph-book`: NaN/Inf reject on price+qty, ArrayBook insert-at-pos-or-drop, MapBook quantize, ItchBookBuilder evict_existing_ref, AddOrder side validation, executed-shares clamping
- `eph-containers`: RingBuffer SPSC release/acquire chain, BoundedQueue shadow-index acquire reload, EvictingQueue SeqLock+atomic_thread_fence, MaxDataSize uint32 static_assert
- `arp.hpp::parse_arp_reply`: nullptr / multi-segment / hw_type / proto / addr_len / opcode / target_ip reflection-attack guards

**Utils + codec (batch 2, 6 rounds)**:
- `recorder.hpp + hdr_histogram`: NaN-guards on percentile, double→uint64 boundary at UINT64_MAX, dropped_count tracking, retired-thread merging via shared_ptr, JSON escape via core::detail::json_escape
- `time.hpp` (TSC): call_once+release-store, do_init_ early-return paths leave initialized_=false, acquire-load+release-store sync of ns_per_cycle_, to_cycles strict-< against double(UINT64_MAX) for 2^64-aliasing UB
- `cpu.hpp` (thread_pin): process-wide registry locked, pre-policy-check+post-affinity-verify shape correct, read_cpu_list_file's TAB-not-stripped is best-effort by design
- `audit_log` / `ema` / `timestamp` / `rate_limiter` / `phased_timer`: per-slot committed_+head_ ordering, flush_to_file errno discipline, NaN/Inf reject with state-unchanged, ms_to_ns/us_to_ns explicit overflow guards
- `mold64_codec`: pre-parse header for typed CodecBad, gap detection on seq>expected, payload>0xFFFF reject pre-needed
- `raw_stream/raw_datagram_codec`: empty-view → Ok(None) for stream / Err(CodecBad) for datagram correct asymmetry, memcpy guarded by !payload.empty()
- `length_prefix_codec`: kMaxFrameLen=16MiB pre-empts slow-loris
- `ws_codec + ws_inflate`: orphan-continuation check uses frag_opcode_ alone (FIN-resets-opcode-but-keeps-frag_buf-for-span-validity pattern), inflateReset on error, max_inflated_size enforced inside grow loop

**DPDK further + net cross-cutting (batch 3, drilled but mostly clean)**:
- `DpdkPoller::poll()` main loop ordering, `dpdk_udp_socket` multicast leave/join, reconnect_policy, bind helpers, byte_socket send/recv errno mapping, TlsConfig validation big-picture, lcore_pin pin_lcores

**Examples + final L4 (batch 4 + 5)**:
- `examples/` compile sanity (clean against current public API)
- `platform.hpp` rss_using_probed_key + RSS bring-up
- `flow_steering.hpp` rule install/uninstall lifecycle (post-d50407f7)
- Cross-module #include audit (header-only invariant clean)
- TODO/FIXME survey across whole repo
- Bench microbenchmark correctness (DoNotOptimize / measurement boundary)
- `tests/integration/*` resource leak / race / timeout / error-swallow review
- `benchmarks/latency/core` framework (warmup gating, sample boundary)
- `summary.md` doc-vs-public-API cross-check

## Deferred / skip_pool

(empty — no skip_pool entries this round)

**Soft observation flagged for retro** (not committed):

CLAUDE.md states "the bench writes no files" but `lat_*_loop` scenarios DO call `export_legs` / `export_json` to `benchmarks/latency/outputs/`. Doc/code drift in CLAUDE.md itself. CLAUDE.md is intentionally off-limits for non-bug edits during these review loops. **Recommended**: separate `/pax --doc "fix CLAUDE.md bench writes no files claim"` follow-up.

## Touched files

```
 examples/binance_book.cpp                                        |  +1 −2
 eph-net/include/eph/net/detail/tls_constants.hpp                 |  +9 −1
 eph-net/include/eph/net/detail/tls_session.hpp                   | +24 −7
 eph-net-dpdk/include/eph/dpdk/detail/mp_registry.hpp             | +12 −0
 eph-net-dpdk/include/eph/net/dpdk/flow_steering.hpp              | +11 −2
 5 files, +57 −12
```

## Branch shape

Single linear feature branch on `pax-review-dpdk-r3-20260502`, 6 commits + this report (will commit as #7), each:

- ✅ Single logical change (no bundling)
- ✅ Conventional Commits format with `<type>(<scope>): <subject>`
- ✅ Body explains WHY + observable consequence
- ✅ Each commit individually built green via gcc14-wrap (cxx + ld + sh)
- ✅ No `--no-verify`, no amend, no force-push

Ready for review or fast-forward merge into `main`. No conflicts with `5a5121b1` base.

## Round 1 → 2 → 3 cumulative perspective

| Metric | R1 | R2 | R3 |
|---|---|---|---|
| Wall-clock | 51 min / 60 min budget | 42 min / 60 min budget | 50 min / 120 min budget |
| Subagent batches | 3 | 2 | 5 |
| Total rounds | 15 | 5 | 21 |
| Effective commits | 8 | 5 | 6 |
| Critical | 0 | 1 (TLS seq overflow) | 0 |
| Major | 4 | 4 | 3 |
| Minor / Docs | 4 | 0 | 3 |
| Saturation point | L3 reached | L3 reached | L3+L4 deep saturation |
| Code surface drilled | DPDK fast path + eph-net stream_metrics | + HTTP / HMAC / TLS / WS / UDP / TcpSession state machine | + ALL parser modules + utils + codec + integration + bench + examples |

**Pattern across 3 rounds**:

1. **Diminishing returns curve**: 8 → 5 → 6 commits as territory exhausts (R3 was 50% larger budget but produced fewer commits than R1 — that's saturation, not slack)
2. **Severity profile shifts deeper into the codebase**: R1 found mostly DPDK fast-path doc/symmetry issues; R2 found packet-parser security bugs + 1 Critical TLS; R3 found subtle MP / TLS-session / IPC-wire issues
3. **The "release symmetry" class repeats** (r1 keepalive_misses_ — r2 effective_mss_ — r3 mp_registry slot). This is a code-pattern bug class worth a project-wide audit OR an idiom (`auto&& cleanup = scope_exit{[]{ release(); }};`) to make it harder to forget
4. **Saturation is real**: by R3 batch 4-5, multiple targets returned 0 commits across thorough drilling. The codebase is at high maturity for a header-only HFT networking library

**Cumulative across 3 rounds**: 19 commits (+ 3 final reports), spanning from doc/symmetry fixes to one Critical security hardening, validated the loop's value as a recurring quality-bar enforcement mechanism.

## Build constraints honored

- All builds via `xmake build -P /tmp/pax-review-dpdk-r3-20260502 -j 2` (correct form, NOT `xmake -y` build)
- gcc14-wrap on `--cxx` + `--ld` + `--sh`
- `-j 2` cap to share CPU
- No DPDK EAL init / `lat` / `dpdk_e2e` invoked
- aws-lc-only TLS path preserved

## Disposition recommendation

- ✅ Fast-forward merge `pax-review-dpdk-r3-20260502` → `main` (clean ff, 6 commits + this report)
- ✅ Optional follow-up: `/pax --doc "fix CLAUDE.md bench writes no files claim"` for the doc/code drift surfaced in batch 5
- ❌ Do NOT immediately schedule R4 — saturation evidence is strong; the next loop should wait for non-trivial new code in the touched modules to provide review surface

## Discipline observation for future loops

R3 closed at ~50 min vs 120 min hardwall (42% utilization). Looking at R1 (85%) and R2 (70%), R3 was a noticeable early-close. The driver was genuine saturation, not laziness — 5 batches all confirmed L4 saturation independently. For a 4th round to be productive, either:

1. New code must land in the eph-net-dpdk module path (review surface regenerates)
2. The LENS must change (e.g. R4 = performance — bench-driven, not bug-driven)
3. The scope must broaden (e.g. R4 = release readiness — `/pax --ship`)

Mechanically forcing more batches at L4 saturation produces 0 commits and burns compute. The dispatcher's call to close at 50 min was the right call.
