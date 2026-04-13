# eph-net-dpdk changelog

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
