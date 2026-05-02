---
title: pax --loop --auto review eph-net-dpdk (round 2)
date: 2026-05-02
branch: pax-review-dpdk-r2-20260502
base: main @ 6a0b67eb (post-r1)
loop_window: 06:37:27 → 07:37:27 (1 h hardwall)
mode: subagent (2 batches)
predecessor: .artifacts/pax-review-dpdk-20260502.md (already merged into main)
---

# /pax --loop --auto review eph-net-dpdk — round 2 final report

## Summary

| Metric | Value |
|---|---|
| Total rounds | 5 (across 2 subagent batches) |
| Effective commits | 5 (all real bugs — no doc-only fixes this round) |
| 🔴 Critical | 1 (TLS in-place AEAD seq overflow guard) |
| 🟡 Major | 4 |
| 🔵 Minor | 0 |
| 📎 Reference | 0 |
| Net diff | `+67 / −3` across 5 files |
| Round-1 carryover honored | yes — no re-finds; r1-clean items not re-drilled |
| LENS exhaustion | Bug L1 strong yield (4 Major + 1 Critical); Debt / Cleanup not entered (saturated on Bug fixes alone) |
| Build verification | every commit individually built green via gcc14-wrap on representative test target |
| DPDK EAL runtime | not invoked — pure compile-only (host shared with main repo) |

## Findings (chronological — one row per commit)

| # | Hash | Severity | Type | File | One-line |
|---|------|----------|------|------|----------|
| 1 | `8ff7ebab` | 🟡 Major | fix | `eph-net-dpdk/include/eph/net/dpdk/flow_steering.hpp` | `fd_install` IPC thunk silently mapped any non-17 proto byte to TCP → forged peer could install a TCP 5-tuple rule for SCTP/ICMP/0/garbage. Strict-validate to {TCP, UDP} only |
| 2 | `c96d2a0a` | 🟡 Major | fix | `eph-net-dpdk/include/eph/dpdk/dns.hpp` | `try_parse_dns_packet` dereferenced `mbuf->nb_segs` with no null-guard — fuzzer or caller misuse would segfault. Mirrors `parse_arp_reply`'s existing null-guard for symmetric LAN-parser robustness |
| 3 | `804a2a9e` | 🟡 Major | fix | `eph-net-dpdk/include/eph/dpdk/packet_parse.hpp` | `parse_icmp` embedded-IP+L4 bounds used `pkt_len` (full mbuf incl. 802.3 padding) instead of `ether + ip_total` → attacker could craft trailer bytes posing as a valid forged 4-tuple after the IP-claimed ICMP message ended |
| 4 | `3220593d` | 🔴 Critical | fix | `eph-net/include/eph/net/detail/tls_inplace.hpp` | `TlsInPlaceDecryptor::open_in_place` (DPDK hot path) was missing the `seq_ < 2^32` guard that legacy `TlsDecryptor::decrypt` enforces. Wrap → per-record nonce (iv XOR seq_be) repeats against same key → AES-GCM key-recovery / forgery. Fail-closed |
| 5 | `3c7c2d15` | 🟡 Major | fix | `eph-net-dpdk/include/eph/dpdk/tcp.hpp` | `TcpSession::reset()` left `effective_mss_ / peer_mss_ / peer_mss_negotiated_` stale → between reset and reconnect any telemetry caller observed the dead session's negotiated values (notably ICMP-shrunken post-Frag-Needed MSS). Defense-in-depth pattern matching r1's keepalive symmetry fix |

### By severity

- 🔴 **Critical: 1** — the TLS AEAD seq-overflow guard. Real-world impact: a TLS session sustained at high record rate would silently start reusing nonces against the same AEAD key after 2^32 records, breaking AES-GCM confidentiality / integrity guarantees. Most production TLS sessions never reach 2^32 records, but a high-rate market-data stream could in theory; the legacy decryptor already had this guard, the DPDK in-place fast-path didn't — silent regression of a security invariant.
- 🟡 **Major: 4** — 3 input-handling robustness on packet parsers (rejecting forged / malformed / null inputs that the prior code accepted), 1 reset() symmetry on a defense-in-depth telemetry path.

### Themes

- **Security-class wins** (1 Critical + 3 Major from input-handling): the previous loop drilled the TCP/codec hot path; round 2's L3-targeted drill into adjacent parsers and the in-place TLS decryptor surfaced bugs of a different shape — protocol-boundary input validation that the project's strict-codec ethos should have caught earlier
- **Defense-in-depth pattern reused**: r1 added `reset()` symmetry for keepalive state; r2 catches the parallel oversight on MSS state. Whenever `reset()` is partial, the gap shows up in telemetry between reset and reconnect

## No-commit clean-review rounds (this batch)

These were drilled and confirmed clean; cite them so future rounds don't re-drill:

| Target | Verdict |
|---|---|
| `dpdk/icmp_directory.hpp` pass-2 logic | clean — TOCTOU between weak_ptr lookup and dispatch is properly guarded by mutex; shared_ptr lifetime semantics correct |
| `dpdk/eal_config.hpp` + `build_eal_argv` | clean — argv lifetime well-managed; no shell-quoting issues for typical PCI BDFs / file_prefix |
| `dpdk/multicast.hpp` group_count vs active count | clean — high-water vs active is documented behavior, not a bug |
| `eph-net-dpdk` header organization sweep | clean — no redundant `<rte_*.h>` pulls; no header-cycle risk found at a level worth committing |
| HTTP/1.1 parser (`eph-net/include/eph/net/http/`) | heavily armored — deliberate subset rejection (chunked / TE / cookies / 100-continue), bare LF rejection, 60-bit overflow guard on Content-Length, strict header-value CTL filtering. No actionable findings |
| HMAC-SHA256 typed wrappers | API-level concerns out of scope (no-verify is a documented design choice); `Key` zeroize relies on aws-lc primitives — acceptable |
| TLS session aws-lc adapter | NSE handling correct; SSL_get_error → eph::Error mapping complete; no actionable findings beyond the seq-overflow fix above |
| DPDK TcpSession state machine | one borderline-unreachable CLOSING-on-FIN transition (`tcp.hpp:1496-1503`) only triggered by buggy peer state — not worth touching |
| WS handshake (`eph-net/include/eph/net/ws_handshake.hpp`) | well-defended: `kMaxKeyLen` scratch cap, mandatory-header collision detection, fail-closed unknown-extension. Sec-WebSocket-Accept compare is non-constant-time but the value is server-derived public — acceptable |
| UDP socket multicast (kernel + DPDK) | MEMBERSHIP add/leave on close handled correctly; IPv6 group mapping correct |
| `flow_steering.hpp::find_src_port_for_queue` 2-pass | clean — no starvation under typical hash distributions; bounds correct when `nb_rx_queues == 1` |

## Deferred / skip_pool

(empty — no items deferred this round)

Two minor observations *not* worth committing this loop, surfaced for future awareness:

- **Builder/parser asymmetry on header value CTLs** — the HTTP parser is stricter about embedded CTL/NUL bytes than the builder is; this is debt not a bug (no current call site allows attacker-controlled header values reach `build_http_request`), but a future header-injection threat model should tighten the builder side too. Not actionable now.
- **WS Sec-WebSocket-Accept non-constant-time compare** — value is server-public so timing leak isn't a vulnerability, but a constant-time compare would be a cleanliness win. Not worth touching during a Bug-LENS loop.

## Touched files

```
 eph-net-dpdk/include/eph/dpdk/dns.hpp                |  +8 −0
 eph-net-dpdk/include/eph/dpdk/packet_parse.hpp       | +17 −2
 eph-net-dpdk/include/eph/dpdk/tcp.hpp                | +11 −0
 eph-net-dpdk/include/eph/net/dpdk/flow_steering.hpp  | +19 −1
 eph-net/include/eph/net/detail/tls_inplace.hpp       | +12 −0
 5 files, +67 −3
```

## Branch shape

Single linear feature branch on `pax-review-dpdk-r2-20260502`, 5 commits + this report (will commit as #6), each:

- ✅ Single logical change (no bundling)
- ✅ Conventional Commits format with `<type>(<scope>): <subject>`
- ✅ Body explains WHY + observable consequence (not narration of WHAT)
- ✅ Each commit individually built green via `xmake build -P /tmp/pax-review-dpdk-r2-20260502 -j 2 <test target>` with gcc14-wrap (cxx + ld + sh)
- ✅ No `--no-verify`, no amend, no force-push

Ready for review or fast-forward merge into `main`. No conflicts with `6a0b67eb` base (which already includes r1).

## Round-1 vs Round-2 comparison (efficiency lessons)

| Metric | Round 1 | Round 2 |
|---|---|---|
| Total wall-clock | 51 min (early close) | ~40 min (planned to ~07:25) |
| Subagent batches | 3 | 2 |
| Commits / round | 8 / 15 = 53% | 5 / 5 = 100% |
| Median commit severity | Mixed (fixes + docs) | All fixes (1 Critical, 4 Major) |
| Re-finds avoided | n/a (first round) | yes (carry-over list honored) |
| L3 expansion productive? | yes (kTlsSendDesyncs doc) | yes (TLS seq guard — 🔴 Critical) |

**Key efficiency lessons that paid off** (vs r1 dispatch tuning):
1. **Pre-scoped target list**: dispatch prompt gave subagent #1 explicit unexplored territory (icmp_directory pass-2, flow_steering find_src_port, packet_parse, eal_config, multicast, header sweep). Subagent didn't burn context on broad scoping
2. **Carry-over discipline**: explicit list of r1 fixes + r1-clean items prevented re-drilling and re-finding
3. **L3 expansion at right time**: subagent #2's prompt named G–L cross-cutting targets directly; HTTP / HMAC / TLS / WS / UDP / TcpSession state machine got fast triage. The 🔴 Critical TLS seq-guard came from this exact L3 reach
4. **Tight time cap on batch 2** (07:32:00 hard) prevented context exhaustion

## Build constraints honored

- All builds via `xmake build -P /tmp/pax-review-dpdk-r2-20260502 -j 2` (correct flag form, NOT `xmake -y` — that flag was invalid for build subcommand and r1 dispatcher hit it once)
- gcc14-wrap on `--cxx` + `--ld` + `--sh` per `feedback_dpdk_linker_wrapper.md`
- One mid-batch xmake config reset detected and recovered (subagent #1 re-ran `xmake f` — same pattern as r1)
- `-j 2` cap to share CPU
- No DPDK EAL init / `lat` / `dpdk_e2e` invoked
- aws-lc-only TLS path preserved — no OpenSSL fallback proposed (the seq-overflow guard fix uses the same aws-lc primitives the legacy decryptor uses)
