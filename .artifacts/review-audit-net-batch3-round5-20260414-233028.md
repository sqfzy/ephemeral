# Net Audit — Batch 3, Round 5 (2026-04-14 23:30 CST)

**Dimension**: Magic numbers and buffer sizing constants scattered inline
across kernel/DPDK backends and WS/HTTP handshake helpers.

## Findings

### LOW-1 — Scattered magic-number buffer sizes across kernel TcpStream

- **File**: `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp`
- **Sites**:
  - `:174  uint8_t tmp[4096];`     TlsWsSink recv scratch
  - `:668  uint8_t sink[4096];`    poll_once_ no-sink drain
  - `:774  uint8_t scratch[1024];` drain_codec_ auto-response OutputBuffer
- **Defect**: three independent magic numbers with no per-site justification
  or common constant. A maintainer tuning RX burst size has to hunt through
  the file to understand which size is which.
- **Severity**: LOW — readability only.
- **Fix**: hoist to named `constexpr` constants at file/namespace scope with
  comments explaining the sizing rationale (handshake scratch vs drain
  sink vs codec auto-response OutputBuffer).

### LOW-2 — Same pattern in `udp_socket.hpp`: `uint8_t scratch[64]`

- **File**: `eph-net-kernel/include/eph/net/kernel/udp_socket.hpp:311`
- **Defect**: 64-byte auto-response OutputBuffer. Same magic-number pattern.
- **Severity**: LOW.
- **Fix**: bundle with LOW-1.

## Action plan

Bundle both LOWs into one commit: introduce namespace-scope
`constexpr std::size_t` constants with clear names and comments. No
behavior change; tests covering the paths still pass.
