# eph-net-dpdk changelog

## [Unreleased] — Production-hardening sweep (round 2, 2026-04-22)

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

## [Unreleased] — Production-hardening sweep (2026-04-22)

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

## [Unreleased] — 5-tuple routing + client source port selection (2026-04-16)

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

## [Unreleased] — Drop dead reconnect field (2026-04-14)

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

## [Unreleased] — Phase 9 Recovery (2026-04-10)

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
