# Changelog

All notable changes to `eph-dpdk` are documented in this file.
Format follows [Keep a Changelog](https://keepachangelog.com/).
The project does not yet tag releases, so all entries are in `[Unreleased]`.

## [Unreleased]

### Added

- **UDP unicast sender** (`udp.hpp`): `UdpSender` built on a precomputed
  `UdpPacketTemplate` (42-byte Eth+IP+UDP header). Supports `send()` /
  `send_batch()` (up to 32 segments per burst) with optional NIC checksum
  offload and a one-shot `build_udp_packet()` convenience function.
- **UDP multicast receiver** (`multicast.hpp`): `MulticastReceiver` manages NIC
  MAC filters per RFC 1112 and delivers zero-copy UDP payloads via callback.
  Includes `make_moldudp64_adapter()` for eph-itch integration, supports up
  to 8 concurrent groups, and records per-group RX counters.
- **Protocol-aware `Reactor<bool EnableUdp>`** (`reactor.hpp`): template-based
  RX multiplexer that handles TCP and UDP on a shared NIC queue. `Reactor<true>`
  adds `add_udp()` / `set_udp_active()`, while `if constexpr` guarantees
  `Reactor<false>` produces identical codegen to the old TCP-only reactor.
- **Layered parse API** (`packet_parse.hpp`): `parse_ip_header()` returns a new
  `ParsedIpHeader` struct (L2+L3 only) for protocol dispatch, plus
  `parse_tcp_from_ip()` / `parse_udp_from_ip()` for zero-redundancy L4 parsing.
- **UDP flow steering** (`flow_steering.hpp`): `install_flow_rule()` now
  accepts a `FlowProtocol` enum (`Tcp` / `Udp`) so rte_flow rules can match
  either L4 protocol. `FlowRule` is RAII with move semantics.
- **Delayed ACK by default** (`tcp.hpp`): a 40 µs delayed-ACK window with
  piggyback-first semantics closes the Linux TCP receiver slow path. A bare
  ACK is only emitted if the timer fires without an outgoing data segment.
  The timer logic is factored out as `detail::ack_timer_expired()` for unit
  testing without a real DPDK session.
- **ARP refresh for idle connections** (`tcp.hpp`): `set_arp_refresh()` /
  `maybe_refresh_arp()` send a unicast ARP to the gateway at a configurable
  interval to keep switch and gateway ARP caches warm.
- **RFC 5961 RST validation**: incoming RST packets whose sequence number
  falls outside the receive window are ignored rather than closing the
  connection, defending against off-path RST injection.
- **CSPRNG ISN** and random ephemeral port generation using `RAND_bytes` from
  aws-lc, with unbiased modulo over a power-of-2 range.
- **Compile-time named logger factory** (`detail/logger.hpp`): C++20 NTTP
  string literal + `get_logger<LoggerName{"dpdk.tcp"}>()` avoids runtime
  map lookups.
- **Observability across all config and stats types**: every config struct
  (`PlatformConfig`, `TcpConfig`, `UdpConfig`, `DpdkEndpoint`,
  `ConnectorOptions`, `ReactorConfig`, `MulticastConfig`, `DnsConfig`,
  `MulticastGroup`) now implements `validate()`, `warnings()`, `dump()`,
  `to_json()`, and `operator==`. Stats types expose `to_json()` and
  `operator-` for interval-based monitoring.
- **`std::formatter` specializations** for `ConnectionTuple`, `ParsedPacket`,
  `ParsedUdpPacket`, `PacketTemplate`, `FlowRule`, `FlowProtocol`,
  `RxDispatchMode`, `DpdkEndpoint`, `ConnectorOptions`, `MulticastGroup`,
  `MulticastConfig`, `UdpConfig`, `UdpSenderStats`, `Platform::Stats`,
  `TcpConfig`, and `TcpSession<>::Stats`.
- **Extensive test coverage** (~550 test cases across 11 suites) covering
  boundary conditions, JSON escaping, equality edge cases, constexpr
  validation, hash direction symmetry, type traits, and lifecycle behaviour.
  All tests run without DPDK EAL using fake mbufs.
- **Micro-benchmarks** for DNS codec, TCP header construction, UDP
  send/checksum/parse/dispatch, multicast, rte_memcpy vs std::memcpy,
  rte_ring vs BoundedQueue, and pipeline composition.
- **DNS fuzzer** (`fuzzers/fuzz_dns_reply.cpp`) for the DNS response parser.
- **Compile-time static_asserts** verifying TCP/IP/UDP/ARP constant values
  (header lengths, EtherTypes, protocol numbers, SYN options layout).

### Changed

- **Split `net_header.hpp`** (previously ~1250 lines) into three focused
  headers with `net_header.hpp` retained as a 13-line umbrella: `packet_core.hpp`
  (constants, byte order, checksum, `ConnectionTuple`), `packet_parse.hpp`
  (`ParsedIpHeader`, `ParsedPacket`, `ParsedUdpPacket` + parse functions), and
  `packet_template.hpp` (`PacketTemplate`, `UdpPacketTemplate`). Fully backward
  compatible.
- **`Reactor` templated** as `template <bool EnableUdp = false>`. Existing
  `Reactor` usage continues to work as `Reactor<>` or `Reactor<false>`.
  `ReactorConfig` extracted to a standalone struct so formatters do not
  depend on the template parameter.
- **`ParsedUdpPacket` moved** from `multicast.hpp` to `packet_parse.hpp` in
  the `net::` namespace. `multicast.hpp` provides a `using` alias for
  backward compatibility.
- **`parse_packet()` and `parse_udp_packet()` rewritten** to delegate to the
  layered API (`parse_ip_header` + `parse_tcp_from_ip` / `parse_udp_from_ip`).
  Behaviour is unchanged.
- **`internet_checksum()` is `constexpr`**: fixed headers can now have their
  checksum verified at compile time via `static_assert`.
- **Ephemeral port generation deduplicated**: a single implementation in
  `net::` is reused by `dns.hpp` and `connector.hpp` (previously duplicated
  across both modules).
- **`ConnectionTuple` comment** corrected from "TCP connection" to
  "network connection" — the struct is protocol-agnostic and shared with UDP.
- **DNS resolver** validates `RAND_status()` before attempting to use the
  CSPRNG, and standardizes `getaddrinfo` hints across the codebase.
- **Unicast/broadcast ARP** is validated on the receiving side against
  opcode, hw/proto type lengths, and target IP to guard against malformed
  or hostile packets.
- **`eal_cleanup()` returns `bool`** and `[[nodiscard]]` is applied to
  `eal_init`, `eal_cleanup`, compile-time utilities in `platform.hpp`, all
  pure functions in `dns.hpp`, `arp.hpp`, and `net_header.hpp`, and to
  equality operators on config structs.
- **Build system**: `xmake.lua` made modular so tests and benchmarks live
  inside the subproject. ARM64 builds skip the x86-only `-mssse3` flag and
  rely on DPDK's NEON `rte_memcpy` path. `RTE_FORCE_INTRINSICS` is forced
  via `add_defines` and `rte_config.h` is force-included.
- **Logger factory extraction**: `detail/logger.hpp` is now a standalone
  lightweight header so `eal.hpp` and `platform.hpp` can create loggers
  without including DPDK packet headers.

### Fixed

- **`UdpSender::send_batch` stats bug**: when some segments failed to build,
  `mbufs[]` indices diverged from `segs[]` indices, causing incorrect
  `tx_bytes` accounting. Now tracked via a parallel `built_lens[]` array.
- **`UdpSender::send()` error logs** now include `payload_len`, `port_id`,
  and `queue_id` context (previously logged only the error type).
- **`connect(Platform&, ...)` overloads** now use `platform.port_id()`
  instead of `opts.platform.port_id`, which could be stale or mismatched
  when reusing an existing Platform.
- **TCP ISN regenerated on every `connect()`**: a timed-out handshake
  previously left stale `snd_nxt_` / `snd_una_` values that bled into the
  next connection attempt.
- **`ack_delay_cycles_()` retries until TSC is calibrated** on startup,
  preventing a corner case where the delayed-ACK timer used an invalid
  cycle count for the first few packets.
- **`write_syn_options` nodiscard warning** suppressed on the legitimate
  call site inside `PacketTemplate::build_packet`.
- **`max_rx_burst`** is now included in `TcpConfig::dump()` / `to_json()`.
- **Production hardening pass**: 10 critical/high-severity fixes across
  ARP, DNS, TCP, and platform code (bounds checks on malformed DNS names,
  iteration caps on pointer chains, reorder-buffer overflow detection,
  mbuf free on all error paths).
- **DPDK RST validation** follows RFC 5961 §3.2 to prevent off-path attackers
  from closing sessions with spoofed RSTs.

### Removed

- **Stale bench history artefacts** (`.bench/` directories under subprojects).
- **Top-level `METRICS.md`** (replaced by per-module benchmark reports).
- **`arp::detail::format_mac` wrapper** (callers use `net::format_mac` directly).
- **Duplicate `UdpHeader` / `kUdpHeaderLen`** definitions previously present
  in both `multicast.hpp` and `dns.hpp`; unified in `packet_core.hpp`.
- **Opt-in delayed-ACK / TCP timestamp flags** — delayed ACK is now the
  default behaviour and the old feature flags were removed.
