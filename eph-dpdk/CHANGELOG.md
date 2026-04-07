# Changelog

All notable changes to eph-dpdk are documented in this file.
Format follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Added
- **UDP unicast sender** (`udp.hpp`): `UdpSender` with precomputed `UdpPacketTemplate` for deterministic-latency TX. `send()` / `send_batch()` with NIC checksum offload support. `build_udp_packet()` convenience function for one-shot sends.
- **Reactor UDP dispatch**: `Reactor<true>` supports TCP + UDP on a shared RX queue. `add_udp()` registers UDP entries with 4-tuple matching. `if constexpr` guarantees `Reactor<false>` has zero UDP code overhead.
- **Layered parse API** (`packet_parse.hpp`): `parse_ip_header()` for L2+L3 protocol dispatch, `parse_tcp_from_ip()` / `parse_udp_from_ip()` for zero-redundancy L4 parsing from pre-parsed IP header.
- **UDP flow steering**: `install_flow_rule()` now accepts `FlowProtocol` enum (`Tcp` / `Udp`) for protocol-specific 4-tuple rules. Default `Tcp` preserves backward compatibility.
- **`UdpConfig::dump()` / `to_json()`** for observability consistency with TCP types.
- **`ParsedIpHeader`** struct for minimal L2+L3 parse result (protocol dispatch without L4 parsing).

### Changed
- **Split `net_header.hpp`** (1250 lines) into 3 focused files:
  - `packet_core.hpp` (~450L): constants, byte order, checksum, ConnectionTuple
  - `packet_parse.hpp` (~390L): all Parsed types + parse functions + layered API
  - `packet_template.hpp` (~450L): PacketTemplate + UdpPacketTemplate
  - `net_header.hpp` retained as 13-line umbrella header (fully backward compatible)
- **`Reactor` templated** as `template <bool EnableUdp = false>`. Existing `Reactor` usage continues to work as `Reactor<>` or `Reactor<false>`.
- **`ReactorConfig` extracted** from `Reactor` inner class to standalone struct (template-independent).
- **`ParsedUdpPacket` moved** from `multicast.hpp` to `packet_parse.hpp` (`net::` namespace). `multicast.hpp` provides `using` alias for backward compatibility.
- **`ConnectionTuple` comment** corrected from "TCP connection" to "network connection" (protocol-agnostic).
- `parse_packet()` and `parse_udp_packet()` refactored internally to use the layered API (behavior unchanged).

### Fixed
- **`send_batch` stats bug**: when some segments failed to build, `mbufs[]` indices diverged from `segs[]` indices, causing incorrect `tx_bytes` accounting. Fixed with `built_lens[]` tracking array.
- **`send()` error logs** now include `payload_len`, `port_id`, `queue_id` context (was missing).
