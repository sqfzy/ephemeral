# eph-net-dpdk changelog

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
