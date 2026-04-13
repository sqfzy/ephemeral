# Code Audit Report: eph-net-dpdk

## Overview
- **Date:** 2026-04-13 05:58 UTC
- **Scope:** Full module audit of `eph-net-dpdk` (DPDK kernel-bypass networking backend)
- **Code size:** 21 production headers (9,306 lines), 23 test files (11,491 lines)
- **Audit dimensions:** Correctness, Security, Performance, Observability, Testing, Design + Architecture/Pattern consistency/Tech debt

## Project Health Summary
- 🔴 Critical: 2
- 🟡 Major: 9
- 🔵 Minor: 7
- 💬 Nit: 3
- **Overall:** Solid low-level DPDK implementation with strong legacy test coverage. Two confirmed bugs (zero-length UDP rejection, UDP template overflow) and several defense-in-depth gaps in packet parsing. The v3.3 wrapper layer (DpdkTcpStream, DpdkUdpSocket, DpdkPoller) is significantly under-tested at the unit level relative to the legacy primitives.

---

## Technical Debt Inventory

| # | Severity | Dimension | Description | Location | Impact | Recommended Skill |
|---|----------|-----------|-------------|----------|--------|-------------------|
| 1 | 🔴 Critical | Correctness | Zero-length UDP datagrams silently rejected despite RFC 768 comment | udp_socket.hpp:306 + packet_parse.hpp:298 | Data loss for zero-len UDP | /fix |
| 2 | 🔴 Critical | Correctness | UDP template uint16_t overflow on large payload_len | packet_template.hpp:344 | Buffer overrun or silent truncation | /fix |
| 3 | 🟡 Major | Security | IP header IHL not validated against packet length | packet_parse.hpp:70-71 | OOB read on malformed IHL | /fix |
| 4 | 🟡 Major | Security | TCP data offset (doff) has no upper-bound or pkt_len cross-check | packet_parse.hpp:184-185 | OOB read on crafted packets | /fix |
| 5 | 🟡 Major | Security | DNS resolver uses 16-bit transaction ID (brute-forceable) | dns.hpp:559-562 | Cache poisoning on shared network | /improve |
| 6 | 🟡 Major | Security | ARP replies accepted without sender MAC validation | arp.hpp:176-179 | ARP spoofing on shared L2 | /improve |
| 7 | 🟡 Major | Design | Dangling Poller pointer in DpdkTcpStream destructor | tcp_stream.hpp:505-509 | Use-after-free if Poller destroyed first | /refactor |
| 8 | 🟡 Major | Correctness | Frame callback silently truncates >64KB frames to uint16_t | tcp_stream.hpp:754-755 | Silent data loss | /fix |
| 9 | 🟡 Major | Testing | Checksum functions (internet/TCP/UDP) completely untested | packet_core.hpp:198-269 | Undetected checksum bugs | /test |
| 10 | 🟡 Major | Testing | v3.3 wrapper types minimally tested (25 tests vs 630 legacy) | tcp_stream.hpp, udp_socket.hpp, poller.hpp | Regressions in public API | /test |
| 11 | 🟡 Major | Testing | StreamConfig/UdpConfig validation untested | config.hpp | Invalid configs accepted silently | /test |
| 12 | 🔵 Minor | Correctness | MbufView::trim_front skips null-check in else branch | mbuf_view.hpp:61 | UB if MbufView(nullptr, N>0) | /fix |
| 13 | 🔵 Minor | Security | UDP length field not cross-validated with IP total length | packet_parse.hpp:287-289 | Accepting malformed packets | /fix |
| 14 | 🔵 Minor | Observability | Platform config warnings() never auto-logged | platform.hpp:163-193 | Misconfig goes unnoticed | /improve |
| 15 | 🔵 Minor | Consistency | Poller hash collisions silently dropped without counter | poller.hpp:269-282 | Unobservable packet loss | /improve |
| 16 | 🔵 Minor | Correctness | Inconsistent atomic memory ordering on last_rx_burst_tsc | tcp.hpp:1197 vs 1211 | Visibility gap (relaxed vs release) | /fix |
| 17 | 🔵 Minor | Robustness | Null mempool only logged, not prevented from operating | tcp.hpp:386-390 | Deferred failure with unclear error | /fix |
| 18 | 🔵 Minor | Design | IP ID increment not thread-safe (doc says single-thread, no assert) | packet_template.hpp:120,213,356 | Silent corruption if misused | /improve |
| 19 | 💬 Nit | Robustness | Multicast burst array unconditionally allocates 512 on stack (4KB) | multicast.hpp:645 | Stack pressure in shallow contexts | — |
| 20 | 💬 Nit | Consistency | RxDispatcher vs DpdkPoller use different hash widths (64 vs 32-bit) | rx_dispatcher.hpp:84 vs poller.hpp:92 | Maintenance confusion | — |
| 21 | 💬 Nit | Documentation | DNS label encoder accepts trailing dots silently | dns.hpp:183-203 | Unconventional but valid QNAME | — |

---

## Detailed Findings

### 🔴 #1: Zero-Length UDP Datagrams Silently Rejected

**File:** `packet_parse.hpp:298` + `udp_socket.hpp:306`
**Type:** Correctness
**Description:** `parse_udp_from_ip()` only sets `result.payload` when `payload_len > 0`, leaving it as nullptr for zero-length datagrams. `DpdkUdpSocket::process_burst_()` then checks `parsed.payload == nullptr` and drops the packet. The comment on line 304 says "Accept zero-length payloads per RFC 768" — but the code does the opposite.

**Suggestion:** Always set the payload pointer to the position past the UDP header, even for zero-length payloads:
```cpp
// packet_parse.hpp:296-300
uint16_t payload_offset = udp_offset + kUdpHeaderLen;
result.payload_len = udp_len - kUdpHeaderLen;
result.payload = data + payload_offset;  // Always set, even if payload_len == 0
```

---

### 🔴 #2: UDP Template uint16_t Overflow on Large Payload

**File:** `packet_template.hpp:344`
**Type:** Correctness
**Description:** `const uint16_t total_len = kUdpAllHeadersLen + payload_len;` — both operands are `uint16_t`. If `payload_len` is near 65535, the sum overflows silently. The TCP equivalent at `build_packet()` correctly uses `uint32_t total_len32` for overflow detection, but the UDP path lacks this.

**Suggestion:** Mirror the TCP pattern:
```cpp
const uint32_t total_len32 = static_cast<uint32_t>(kUdpAllHeadersLen) + payload_len;
if (total_len32 > UINT16_MAX) return 0;
const uint16_t total_len = static_cast<uint16_t>(total_len32);
```

---

### 🟡 #3: IP Header IHL Not Validated Against Packet Length

**File:** `packet_parse.hpp:70-71`
**Type:** Security (defense in depth)
**Description:** `parse_ip_header()` validates `ihl >= kIpv4HeaderLen` (lower bound) but never checks `kEtherHeaderLen + ihl <= pkt_len`. A crafted packet with `IHL=15` (60 bytes) on a 64-byte Ethernet frame passes the parser. Callers (TCP/UDP parsers) do cross-check against `pkt_len`, but the IP parser should be self-contained.

**Suggestion:** Add bounds check after IHL extraction:
```cpp
if (ihl < kIpv4HeaderLen) return {};
if (kEtherHeaderLen + ihl > pkt_len) return {};
```

---

### 🟡 #4: TCP Data Offset No Upper-Bound Check

**File:** `packet_parse.hpp:184-185`
**Type:** Security
**Description:** `tcp_doff` is validated against `kTcpHeaderLen` (20) but has no upper bound or cross-check against `pkt_len`. A malformed `doff=15` (60 bytes) combined with a short frame could allow the payload pointer at line 202 to reference memory past the packet. Line 199 (`kEtherHeaderLen + ip_total > pkt_len`) catches some cases, but relies on the IP total_length field being honest.

**Suggestion:**
```cpp
if (tcp_doff < kTcpHeaderLen || tcp_doff > 60) return {};
if (tcp_offset + tcp_doff > pkt_len) return {};
```

---

### 🟡 #5: DNS 16-bit Transaction ID

**File:** `dns.hpp:559-562`
**Type:** Security
**Description:** 16-bit TX ID with 3 retries gives ~2^16 guessing space. On a shared network segment, an attacker can achieve meaningful collision probability with ~256 spoofed responses. The code uses `getrandom(2)` for randomness (good), but the ID space is inherently small.

**Suggestion:** For HFT production, consider: (1) randomize source port per query for additional entropy, (2) add HMAC-based response validation, or (3) use DNS-over-TLS. Document the threat model — if the DNS resolver only runs on a trusted management interface, the risk is acceptable.

---

### 🟡 #6: ARP Without Sender MAC Validation

**File:** `arp.hpp:176-179`
**Type:** Security
**Description:** `parse_arp_reply()` validates opcode, hw_type, proto_type, and target IP, but does not validate the sender MAC against any expected value. An attacker on the same L2 segment can redirect traffic via gratuitous ARP.

**Suggestion:** Add optional `expected_gateway_mac` parameter. In HFT environments, the gateway MAC is typically static and known — pre-configure and validate.

---

### 🟡 #7: Dangling Poller Pointer in DpdkTcpStream

**File:** `tcp_stream.hpp:505-509`
**Type:** Design
**Description:** `attached_to_` is a raw pointer to a `DpdkPoller`. If the Poller is destroyed before the stream, the destructor dereferences a dangling pointer. The stream is non-movable (line 514), which helps, but there's no compile-time or runtime enforcement that Poller outlives its pollables.

**Suggestion:** Document the lifetime contract explicitly. Consider a debug-mode weak_ptr or a Poller destructor that asserts `entries_.empty()`. The existing `notify_detached_()` callback is the right mechanism — just needs enforcement.

---

### 🟡 #8: Frame Callback Truncates >64KB to uint16_t

**File:** `tcp_stream.hpp:754-755`
**Type:** Correctness
**Description:** The `on_message` callback takes `uint16_t` length, so frames larger than 65535 bytes are silently truncated. For TCP streams carrying large application messages (e.g., market data snapshots), this loses data without any indication.

**Suggestion:** Log a warning when truncation occurs. Or change the callback signature to use `std::size_t`. If the 16-bit interface is intentional (HFT: no message should be >64KB), document and assert.

---

### 🟡 #9-11: Test Coverage Gaps

**Type:** Testing

- **#9: Checksum functions untested** (`packet_core.hpp:198-269`). `tcp_checksum()`, `udp_checksum()`, `pseudo_header_sum()` have no direct unit tests. Odd-length payloads, zero-length segments, and RFC 1071 edge cases are unverified.
- **#10: v3.3 wrappers under-tested**. 25 v3.3 tests vs 630 legacy tests. `DpdkTcpStream::create()` happy path, `DpdkPoller::poll()` with real burst dispatch, `DpdkUdpSocket::send_to()` are untested at the unit level — relying entirely on E2E tests.
- **#11: Config validation untested**. `StreamConfig` and `UdpConfig` WebSocket fields, TLS settings, and proxy rejection logic have no unit tests.

**Suggestion:** → `/test` to fill these gaps.

---

### 🔵 #12-18: Minor Issues

| # | File:Line | Issue |
|---|-----------|-------|
| 12 | `mbuf_view.hpp:61` | `data_ += n` without null check in else branch. Safe by invariant (null → length 0), but fragile. Add `if (data_)` guard. |
| 13 | `packet_parse.hpp:287-289` | UDP length not cross-validated with IP total_length. Accepts malformed packets. |
| 14 | `platform.hpp:163-193` | `PlatformConfig::warnings()` exists but is never called from `Platform::create()`. Misconfigs go unnoticed. |
| 15 | `poller.hpp:269-282` | Hash collisions cause silent packet drops. Add a collision counter for observability. |
| 16 | `tcp.hpp:1197 vs 1211` | `last_rx_burst_tsc_` stored with `relaxed` in poll_rx but `release` in setter. Use consistent `release`. |
| 17 | `tcp.hpp:386-390` | Null mempool logged as error but session still constructed. Subsequent ops fail with unclear errors. |
| 18 | `packet_template.hpp:120,213,356` | `ip_id++` not thread-safe. Doc warns, but no `assert(single_thread)` in debug mode. |

---

## Architecture Assessment

**Strengths:**
- Clean separation between legacy DPDK primitives (`eph::dpdk::*`) and v3.3 wrappers (`eph::net::dpdk::*`)
- Zero-copy design maintained throughout: `MbufView` exposes raw mbuf data to codecs without memcpy
- Concept-driven: `DpdkTcpStream` and `DpdkUdpSocket` satisfy `Stream` / `Datagram` concepts with no virtual dispatch
- `PacketTemplate` pre-computes header bytes — only IP ID and lengths change per-packet
- Comprehensive legacy test suite covers TCP state machine, packet parsing, and adversarial inputs

**Concerns:**
- The legacy layer (`eph::dpdk::TcpSession`, `UdpSocket`) and v3.3 wrappers (`DpdkTcpStream`, `DpdkUdpSocket`) overlap significantly. The wrappers delegate to legacy types, creating a two-layer architecture where bugs can hide at the boundary.
- `RxDispatcher` (legacy) and `DpdkPoller` (v3.3) serve similar roles but with different dispatch strategies. The relationship could be clearer.
- Test coverage is heavily skewed toward legacy primitives. The v3.3 public API surface — which is what applications actually use — has minimal unit-level coverage.

---

## Highlights

- `tcp.hpp:900-924`: RST validation implements RFC 5961 §3.2 correctly with wraparound-aware sequence comparison. Well-documented with specific RFC reference.
- `packet_core.hpp:221-224`: Odd-length checksum handling correctly zero-initializes the word before partial memcpy.
- `flow_steering.hpp:222`: RETA size correctly capped to array capacity before the loop — prevents OOB even with unusual NIC drivers.
- `eal.hpp:42-66`: EAL double-init guard with CAS is clean and handles init failure by rolling back the flag.

---

## Recommended Action Plan

1. **[Critical]** Fix zero-length UDP rejection + UDP template overflow → `/fix`
2. **[Major]** Harden packet parsers (IHL/doff bounds) → `/fix`
3. **[Major]** Fill checksum + v3.3 API test gaps → `/test`
4. **[Major]** Document DNS/ARP threat model or harden → `/improve`
5. **[Minor]** Add observability (poller collision counters, config warnings) → `/improve`
6. **[Minor]** Consistency fixes (atomic ordering, null guards) → `/refactor`

## Follow-up
- Use `/discuss` to prioritize tech debt items against roadmap
- High-priority items (1-2) can be fixed with `/fix`
- Test gaps (3) should be filled with `/test`
- Re-audit with `/review audit target: eph-net-dpdk` after fixes
