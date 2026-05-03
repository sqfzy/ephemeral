# DPDK TCP implementation guide

Reference for operators, protocol reviewers, and anyone touching
`eph::dpdk::TcpSession<ReorderSlots>` or its
`eph::net::dpdk::DpdkTcpStream<C, EnableTls>` wrapper. Generated from the
code under `eph-net-dpdk/include/eph/dpdk/tcp.hpp` and
`eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp`.

The code here is intentionally NOT a full TCP implementation — the
HFT workload assumes a direct-colo network path with negligible loss,
so everything Linux's TCP stack does to cover lossy internet links
is deliberately absent. This guide is structured around **what is
done**, **what is deliberately not done**, and **why that trade-off
is appropriate for the workload**.

---

## 1. State machine

`TcpSession::state_` is an `eph::net::TcpState` enum with the standard
RFC 793 states. The transition graph below shows only the edges
actually exercised in production; some theoretically possible edges
(LISTEN-side passive open, SYN-RCVD, simultaneous close) exist in
the enum but are unreachable because this implementation is a
**client-only** connector — it only makes outbound connects.

```
         ┌──────────────┐
         │    Closed    │◄──────────────┐
         └──────┬───────┘               │
                │ connect()             │ RST received
                │ → send SYN            │ (RFC 5961 guarded)
                ▼                       │
         ┌──────────────┐               │
         │  SynSent     │───────────────┤
         └──────┬───────┘               │ connect timeout
                │ SYN-ACK recv          │
                │ → send ACK            │
                │ (MSS negotiated)      │
                ▼                       │
         ┌──────────────┐               │
         │ Established  │───────────────┤
         └──────┬───────┘               │
                │ ┌─ close() → send FIN ────┐
                │ │                         │
                │ ▼                         ▼
         ┌──────────────┐          ┌──────────────┐
         │  FinWait1    │          │  CloseWait   │
         └──────┬───────┘          └──────┬───────┘
                │ ACK recv                │ close() → send FIN
                │                         │
                ▼                         ▼
         ┌──────────────┐          ┌──────────────┐
         │  FinWait2    │          │   LastAck    │
         └──────┬───────┘          └──────┬───────┘
                │ FIN recv                │ ACK recv
                │ → send ACK              │
                ▼                         ▼
         ┌──────────────┐                 │
         │  TimeWait    │─────────────────┤
         └──────┬───────┘                 │
                │ 2MSL elapsed            │
                └─────────────────────────┘
                                          │
                                          ▼
                                      Closed
```

**Test-only state-injection hooks** (`inject_state_for_testing`,
`inject_send_seq_for_testing`, `inject_recv_seq_for_testing`) let
tests fast-forward into any state without running the real handshake;
they live behind `EPH_DPDK_TCP_STREAM_TEST_HOOKS` and the per-file
test-hook macro.

### 1.1 3-way handshake

Driven by `TcpSession::connect(timeout)`. Sends a SYN, retransmits
every 200 ms (`kSynRetransmitInterval` — **not** the full RFC 6298
RTO: HFT peers respond within one RTT or something is wrong and we
should surface the error fast). Processes the first SYN-ACK, clamps
`effective_mss_` to `min(local_mss, peer_mss_from_option)`, and
emits the final ACK.

### 1.2 FIN / RST handling

- `close()` in `Established` → FIN + transition to FinWait1.
- `close()` in `CloseWait` → FIN + transition to LastAck.
- Peer RST anywhere → RFC 5961 in-window validation
  (`(seq - rcv_nxt_) in [0, rcv_wnd)`), then transition to Closed.
  Unsolicited / out-of-window RSTs are dropped silently.
- Peer FIN in `Established` → CloseWait; the application must call
  `close()` to complete the teardown.

`TcpSession::reset()` force-transitions to Closed and emits a
best-effort RST. Called from `DpdkTcpStream::handle_rx_session_error_`
when `process_rx` returns `Error::Disconnected` (reorder-buffer overflow
being the production-observed trigger; see §3).

### 1.3 TIME_WAIT and 2MSL

Client-initiated closes end in TIME_WAIT for the conventional 2MSL
window. The implementation holds the session structure alive (and
keeps its 5-tuple in the Poller's routing table) until the stream's
destructor runs — applications that rapid-reconnect on the same
src_port should rotate ports (see `DpdkPoller::pick_src_port()`)
to avoid stepping on their own TIME_WAIT entries kernel-side.

---

## 2. Reorder buffer

`TcpSession<ReorderSlots=64>::reorder_buf_[]` is a fixed-size
linear-scan array of `ReorderEntry` records, each carrying a
`std::array<uint8_t, kDefaultMss>` payload. 64 slots × 1460-byte
MSS = ~93 KiB per session.

```
  process_rx receives segment with seq = S
         │
         ▼
  S == rcv_nxt_ ?
         │
     ┌───┴───┐
    yes      no
     │       │
     │       │
     │       ▼
     │    seq_after(S, rcv_nxt_) ?
     │       │
     │   ┌───┴──────┐
     │  yes        no
     │   │          │
     │   │          ▼
     │   │     duplicate / past segment — drop + DEBUG log
     │   │
     │   ▼
     │  reorder_count_ < ReorderSlots ?
     │   │
     │  ┌┴─────┐
     │ yes     no
     │  │      │
     │  │      ▼
     │  │   reorder_overflows++ → abort_rx_cleanup() →
     │  │   return Error::Disconnected
     │  │
     │  ▼
     │  reorder_buf_[reorder_count_++] = copy of payload
     │  (reorder_hits++)
     │
     ▼
  deliver in-order, rcv_nxt_ += len
         │
         ▼
  drain_reorder_buf() — linear scan for any buffered entries
  that now match the advanced rcv_nxt_, deliver + compact
```

### 2.1 Overflow semantics

The overflow branch in `tcp.hpp` `process_rx` (search for
`reorder_overflows++`) is the one the Apr-23 stream-layer fix (commit
c90a744) hardens: on overflow, `process_rx` frees the entire burst
via `abort_rx_cleanup` and returns `Error::Disconnected`.
`DpdkTcpStream::process_burst_` observes the error and calls
`handle_rx_session_error_`, which force-transitions to Closed and
bumps `StreamMetric::kRxSessionResets` so the application's reconnect
policy can take over.

Behavioral coverage: `test_dpdk_tcp_stream.cpp`
`DpdkTcpStreamReorderOverflowE2E.RealReorderOverflowDrivesStreamReset`
drives real crafted mbufs through the full stream layer and asserts
the `state → Closed` + `kRxSessionResets == 1` + `reorder_overflows == 1`
chain.

### 2.2 Why linear scan instead of sorted / hashed

With ReorderSlots=64 and typical HFT RX patterns (2–4 TCP streams
per lcore, near-zero reordering on colo links), `drain_reorder_buf`
touches 1-2 slots on average. A hash-indexed structure would pay
the hash overhead on every insert + every drain scan; a sorted
heap adds O(log N) insert cost. For 2-4 concurrent streams, the
constant factor of linear scan wins. If a workload ever runs >16
concurrent streams per lcore, revisit this assumption.

---

## 3. Delayed-ACK

`TcpSession::flush_pending_ack(now_tsc)` is the delayed-ACK driver.
`ack_pending_since_tsc_` stores the TSC at which the session first
owed an ACK it has not yet emitted; `config_.delayed_ack_interval`
controls how long to wait before forcing the ACK out.

**The caller drives the flush.** `DpdkPoller::poll()` invokes it
through the `on_poll_tick_` hook every poll cycle for every
registered Pollable; applications driving `poll_once_` directly
must call `flush_pending_ack` themselves.

Compared with Linux's delayed-ACK the implementation intentionally
lacks:
- **No quickack mode**: Linux switches in/out of delayed-ACK based
  on observed traffic patterns; we keep delayed-ACK uniformly on
  when enabled.
- **No every-other-segment ACK**: RFC 1122 says "at least every
  second full-sized segment"; our implementation trusts the timer
  alone.

Both simplifications are HFT-appropriate: traffic patterns on
direct colo links are stable; the timer alone suffices.

---

## 4. No-retransmit contract

TcpSession deliberately does **not** implement:

- **Data retransmission**: once `send()` returns success, the
  segment is considered delivered. No RTO timer, no dup-ACK
  counting, no fast retransmit.
- **Nagle**: `send()` writes immediately; small writes are not
  coalesced with pending data.
- **SACK**: reorder buffer accepts forward gaps but does not
  negotiate or honor SACK blocks.
- **PAWS timestamps (RFC 1323)**: no TS option on SYN/data.
- **Window scaling (RFC 1323)**: advertised window capped at
  `UINT16_MAX` (65535).
- **Congestion control**: no slow start, no congestion window.

**Only SYN is retransmitted** during connect (200 ms interval,
capped by `connect_timeout`). SYN-ACK from the peer implicitly
ACKs the SYN.

### 4.1 Why not

HFT matching engines are colocated with the exchange (typical
round-trip < 100 µs, loss rate << 0.01 %). The observed cost of
running full TCP recovery logic on the RX hot path is larger
than the cost of a fresh reconnect on the rare loss event.
The session's response to loss is therefore:

```
  loss detected
         │
         ▼
  reorder buffer fills (64 forward segments with no gap-fill)
         │
         ▼
  process_rx returns Error::Disconnected
         │
         ▼
  DpdkTcpStream::handle_rx_session_error_
      → sess_.reset() → state_ = Closed
      → kRxSessionResets++
         │
         ▼
  Application reconnect policy detects state() == Closed
         │
         ▼
  Fresh connection — typically < 2 ms with warm ARP cache
```

Total MTTR: one RTT for SYN + one RTT for SYN-ACK = ~200 µs on
a healthy colo link. Comparable to one RTO in standard TCP, with
no silent latency tail from dup-ACK waiting.

---

## 5. ICMP path-MTU feedback

Optional, opt-in via `DpdkTcpStream::create_and_attach(cfg, platform)`
— that factory registers the stream with `Platform::icmp_registry_`
so router-originated ICMP Type 3 Code 4 (Fragmentation Needed)
messages are dispatched to the owning session regardless of which
RX queue they arrive on.

On receipt, `TcpSession::on_icmp_frag_needed(next_hop_mtu)` clamps
`effective_mss_` down to `next_hop_mtu - IP_HDR - TCP_HDR`. The
new MSS is exposed via `peer_mss_negotiated()` / `effective_mss()`
for diagnostics.

The registry is `shared_ptr`-managed and internally mutex-locked —
safe under any declaration / destruction order between Platform,
Poller, and Stream, and safe under concurrent register / unregister
/ dispatch. See `eph-net-dpdk/include/eph/dpdk/detail/icmp_registry.hpp`.

---

## 6. Keepalive

Opt-in via the user-facing `eph::net::dpdk::StreamConfig::keepalive`
(`KeepaliveConfig{.interval, .probes}`, default `interval == 0` = disabled).
`DpdkTcpStream::create_and_attach` lowers this into the wire-level
`TcpConfig::keepalive_interval / keepalive_probes` at factory time.
When positive, the caller must drive `tick_keepalive(now_tsc)`:

- **DpdkPoller production**: `poll()` invokes `on_poll_tick_` on
  every registered Pollable on every cycle; the Stream forwards
  to the session's tick.
- **Single-stream direct drive**: the application must tick
  explicitly; see `docs/poller-guide.md`.

The tick behaves as:
1. No RX baseline yet → anchor to `now_tsc` (fresh connections
   don't probe immediately).
2. Within one interval of last RX → no-op.
3. Rate-limited: one probe per interval window.
4. `keepalive_misses_ >= keepalive_probes` → declare dead
   (`state_ = Closed`); no further probe.
5. Otherwise emit a probe, `keepalive_misses_++` + `stats_.keepalive_probes_sent++`.

An incoming matching segment resets `keepalive_misses_ = 0`. See
`test_tcp_close_reset.cpp` `KeepaliveProbeExhaustionTransitionsToClosed`
for the exhaust semantics.

---

## 7. Telemetry surface

Stream-level (`DpdkTcpStream::metric(StreamMetric m)`):

- `kBytesSent` / `kBytesRecv` / `kFramesDecoded` — core I/O
- `kTcpResetsReceived` — peer-initiated RSTs observed
- `kTcpOutOfOrderSegments` / `kTcpReorderBufferHits` / `kTcpReorderBufferOverflows` — reorder telemetry
- `kTcpKeepaliveProbesSent` — keepalive count
- `kTcpKeepaliveSendFailures` — keepalive tick where the probe couldn't be transmitted (mempool exhausted / tx_burst returned 0)
- `kTcpMssNegotiationApplied` — SYN-ACK MSS clamp events
- `kIcmpFragNeededReceived` — path-MTU feedbacks acted on
- `kRxSessionResets` — stream-layer initiated resets

Session-level (`TcpSession::stats()`):
`tx_packets / rx_packets / tx_bytes / rx_bytes / acks_sent /
out_of_order / reorder_hits / reorder_overflows / max_gap_size /
gap_histogram / resets_received / keepalive_probes_sent`.

Both are `alignas(64) std::atomic<uint64_t>` (on `DpdkTcpStream`;
session stats are plain — single-lcore by design). Publish via
`eph::net::publish_metrics<Stream, Sink>(stream, sink, tags)`.

---

## See also

- [`../eph-net-dpdk/README.md`](../eph-net-dpdk/README.md) — module
  entry point + thread model diagram
- [`dpdk-udp-design.md`](dpdk-udp-design.md) — UDP backend design
  deltas vs the kernel backend
- [`dpdk-setup.md`](dpdk-setup.md) — hugepages, vfio-pci, EAL
- [`poller-guide.md`](poller-guide.md) — how the Poller drives
  `on_poll_tick_` and `process_burst_`
- [`observability-guide.md`](observability-guide.md) —
  `publish_metrics` + MetricsSink
- `eph-net-dpdk/include/eph/dpdk/tcp.hpp` — source of truth
- `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp` — stream
  wrapper
