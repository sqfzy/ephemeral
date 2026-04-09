# eph-transport

Header-only C++23 WebSocket / raw TCP transport with TLS 1.3 encryption,
designed for HFT market-data feeds and low-latency trading applications.
Unifies kernel-socket and DPDK poll-mode backends behind a single generic
`TcpTransport` concept and offers three transport variants with different
threading models, all sharing the same TLS stack, WebSocket state machine,
and reconnection policy.

## Overview

`eph-transport` is the network transport layer of the `eph` ecosystem.
It sits between a TCP backend (`eph-net` kernel sockets, `eph-dpdk`
poll-mode) and application-level protocol parsers (e.g. FIX, ITCH, JSON
market data). Its responsibilities:

- Establish encrypted WebSocket connections (TCP connect, TLS 1.3
  handshake via aws-lc, RFC 6455 HTTP Upgrade).
- Run the WebSocket state machine: frame encode/decode, masking, ping/pong,
  graceful close, fragmentation reassembly.
- Carry application payloads over TLS 1.3 with a custom AEAD fast path
  (AES-128/256-GCM) that bypasses `SSL_read`/`SSL_write` and touches only
  exported session keys on the hot path.
- Manage connection lifecycle: exponential-backoff reconnection with
  ±25% jitter, state-change callbacks, pluggable frame filters.
- Deliver per-stage latency observability (TSC timestamps, HdrHistogram
  for queue-wait / encode / decrypt / decode / total pipeline).

The library is fully generic:

- Over the TCP backend, through the `TcpTransport` concept from
  `eph-core` — any socket implementation that satisfies the concept
  plugs in unchanged (kernel, DPDK, testing mock, etc.).
- Over the wire framer, through the `MessageFramer` concept —
  `WsFramer` for WebSocket, `RawFramer` for SOH-delimited or
  length-prefixed protocols that manage their own boundaries.

## Transport variants

Three top-level class templates address different latency / threading
tradeoffs. All share `TransportCore` (connection state, TLS handshake,
WS upgrade), `FrameProcessor` / `RxWorker` (RX decode pipeline), and
`ReconnectPolicy` (backoff state machine).

| Class | TX path | RX path | When to use |
|---|---|---|---|
| `Transport` | TX thread + TX SPSC queue | RX thread + RX SPSC queue | Default — non-blocking `send()`/`recv()`, decoupled app thread |
| `DirectTxTransport` | Direct from app thread (no TX queue) | RX thread + RX SPSC queue | Lowest TX latency; async RX |
| `DirectTransport` | Direct from app thread | Direct from app thread (no threads) | Single-threaded event loops: Reactor, io_uring, DPDK poll-mode |

All three are non-movable, non-copyable, RAII-owned via `std::unique_ptr`
returned from the `create(tcp_factory, config)` static factory.

### Threaded `Transport` data flow

```
App thread               TX thread            Network
    |                        |                    |
    |-- send() --> [TX SPSC] --> WS encode -------|
    |                        |    TLS encrypt     |
    |                        |    TCP send        |
    |                                              |
    |-- recv() <-- [RX SPSC] <-- WS decode --------|
    |                        |    TLS decrypt     |
    |                        |    TCP poll        |
    |                        |                    |
                        RX thread
```

`DirectTxTransport` removes the TX thread and TX queue, running
`send()` synchronously on the caller's thread.
`DirectTransport` also removes the RX thread: the caller drives
`poll()` (or `feed_rx()` + `process_pending()`) from its own event loop.

## Directory layout

```
eph-transport/
├── include/eph/transport/
│   ├── transport.hpp              # Transport<>        — threaded variant
│   ├── direct_tx_transport.hpp    # DirectTxTransport<> — direct TX + RX thread
│   ├── direct_transport.hpp       # DirectTransport<>   — threadless
│   ├── presets.hpp                # Default/Small/Large/Evict/Raw aliases
│   ├── transport_types.hpp        # TransportConfig, TransportStats, enums, RttStats
│   ├── reconnect_policy.hpp       # ReconnectPolicy (exponential backoff + jitter)
│   ├── ws_framer.hpp              # MessageFramer adapter for WebSocket
│   ├── raw_framer.hpp             # Pass-through framer
│   └── detail/
│       ├── transport_core.hpp     # Shared connection state (TCP + TLS + config)
│       ├── tx_worker.hpp          # TX thread, TX SPSC queue, TX stats, ping scheduling
│       ├── rx_worker.hpp          # RX thread, RX SPSC queue, RX stats, FrameProcessor host
│       ├── frame_processor.hpp    # WS decode pipeline, fragmentation reassembly
│       ├── frame_filter.hpp       # FrameView / make_twophase_filter
│       ├── websocket.hpp          # RFC 6455 encode/decode, mask cache, opcode helpers
│       ├── http.hpp               # Minimal HTTP/1.1 for WS Upgrade only
│       ├── tls_session.hpp        # TLS 1.3 handshake via aws-lc BIO
│       ├── tls_record.hpp         # TlsRecordCrypto (split enc/dec)
│       ├── tls_encryptor.hpp      # AES-GCM write-side AEAD
│       ├── tls_decryptor.hpp      # AES-GCM read-side AEAD
│       ├── tls_constants.hpp      # TLS record layout, nonce construction
│       ├── message_types.hpp      # TxMessage / RxMessage (cache-aligned)
│       └── logger.hpp             # Shared spdlog logger helper
├── tests/                         # Unit and integration tests (GoogleTest)
├── benchmarks/                    # Micro-benchmarks (nanobench)
├── fuzzers/                       # libFuzzer harnesses (WS decode)
├── docs/ONBOARDING.md             # Developer onboarding guide
├── summary.md                     # Architecture + component map + data flow
└── xmake.lua                      # Build rules (header-only + tests + benches)
```

## Dependencies

- **eph-core** — `TcpTransport` / `MessageFramer` concepts, transport
  error types (`ConnectionError`, `SendError`, `FrameError`), shared
  JSON escape and control-char checks.
- **eph-containers** — `BoundedQueue` and `EvictingQueue` (lock-free
  SPSC queues for TX and RX paths).
- **eph-utils** — `HdrHistogram`, `TSC`, cache-line alignment, CPU
  affinity helpers.
- **spdlog** — leveled logging via `SPDLOG_LOGGER_*` macros, subject to
  `SPDLOG_ACTIVE_LEVEL` compile-time filtering.
- **aws-lc** — TLS 1.3 handshake (libssl-compatible) and AES-GCM AEAD
  (libcrypto-compatible).

All dependencies are propagated as public xmake packages — consumers
link `eph-transport` only.

## Build

`eph-transport` is header-only and part of the top-level `ephemeral_dev`
xmake build.

```bash
# From the repo root
xmake build eph-transport          # no-op, header-only (validates deps)
xmake build test_transport_types   # build a specific test binary
xmake build -g tests               # build all tests
xmake build -g benchmarks          # build all benchmarks
```

The `xmake.lua` in this directory declares:

- `target("eph-transport")` — header-only, public includes, public
  deps on `eph-core`, `eph-utils`, `eph-containers`, and public
  packages `spdlog`, `aws-lc`.
- One test target per file under `tests/`, using the `eph-test` rule
  and adding `eph-net` as a dependency (tests rely on the kernel
  backend for end-to-end assertions).
- One benchmark target per file under `benchmarks/`, using the
  `eph-bench` rule.

Optional compile-time knob: pass `-DEPH_ENABLE_TIMESTAMPS=1` to enable
per-message TSC timestamps and the HdrHistogram latency breakdowns.
Disabled by default — when off, the `tsc` fields in `TxMessage`/`RxMessage`
are unused and histogram calls compile to no-ops.

## Quick examples

### Threaded transport (default)

```cpp
#include <eph/transport/transport.hpp>
#include <eph/transport/presets.hpp>

auto cfg = eph::net::TransportConfig::from_url(
    "wss://stream.binance.com:9443/ws/btcusdt@bookTicker");
if (!cfg) { /* handle URL parse error */ }

cfg->on_message = [](const uint8_t* data, uint16_t len, uint8_t /*op*/) {
    // Called from the RX thread — must be non-blocking.
    process_market_data(data, len);
};

auto factory = [&]() -> std::expected<std::unique_ptr<MyTcp>, std::string> {
    auto tcp = std::make_unique<MyTcp>();
    if (auto r = tcp->connect(std::chrono::milliseconds{3000}); !r)
        return std::unexpected(r.error());
    return tcp;
};

auto result = eph::net::DefaultTransport<MyTcp>::create(
    std::move(factory), *cfg);
if (!result) {
    spdlog::error("connect failed: {}", result.error().message());
    return 1;
}
auto& transport = *result;

// Non-blocking send (enqueued to TX thread)
(void)transport->send(payload.data(), payload.size());

// Or pull-mode receive
transport->recv([](const uint8_t* data, size_t len, uint8_t opcode) {
    // Pointer valid only during this callback.
});

// Windowed statistics (subtract snapshots)
auto s0 = transport->stats();
std::this_thread::sleep_for(std::chrono::seconds{10});
auto delta = transport->stats() - s0;
spdlog::info("rx {:.0f} msg/s, p99 RTT {:.1f}us",
             delta.rx_pps(), delta.rtt.p99_us());

// RFC 6455 graceful close
(void)transport->close_gracefully();
```

### Direct-TX transport (lowest TX latency)

```cpp
#include <eph/transport/presets.hpp>

auto result = eph::net::DirectTxDefaultTransport<MyTcp>::create(
    std::move(factory), *cfg);
auto& dtx = *result;

// TX is synchronous on the calling thread (no SPSC enqueue)
(void)dtx->send(order_bytes.data(), order_bytes.size());

// RX still runs on a background thread; poll the RX queue
dtx->recv([](const uint8_t* data, size_t len, uint8_t) {
    handle_response(data, len);
});
```

Thread-safety note: on `DirectTxTransport` the app thread exclusively
owns `crypto->enc` (write direction), while the RX thread exclusively
owns `crypto->dec` (read direction). Do not call `send()` concurrently
with `stop()`.

### Threadless direct transport (Reactor / DPDK poll-mode)

```cpp
#include <eph/transport/direct_transport.hpp>

auto result = eph::net::DirectTransport<MyTcp>::create(
    std::move(factory), *cfg);
auto& dt = *result;

// Everything runs on the caller's thread
(void)dt->send(data, len);   // encode + encrypt + TCP send, inline
dt->poll();                  // TCP poll + decrypt + decode + deliver

// Or split for Reactor integration:
dt->feed_rx(raw_bytes, len); // accumulate only, no work
dt->process_pending();       // decrypt + decode + deliver for buffered data
```

### Presets (payload / queue depth / framer)

```cpp
// Parameterise any variant on your TCP backend
using Tx  = eph::net::DefaultTransport<MyTcp>;         // 512B / 1024 / WS
using Tx2 = eph::net::SmallTransport<MyTcp>;           //  64B /  256 / WS
using Tx3 = eph::net::LargeTransport<MyTcp>;           // 4096B /  512 / WS
using Tx4 = eph::net::EvictTransport<MyTcp>;           // evicting RX queue
using Tx5 = eph::net::RawTransport<MyTcp>;             // RawFramer (no WS)
using Tx6 = eph::net::DirectTxDefaultTransport<MyTcp>; // DirectTx + defaults
using Tx7 = eph::net::DirectDefaultTransport<MyTcp>;   // Direct + defaults
```

### Batch frame filter (multi-symbol dedup)

```cpp
// Deliver only the latest frame per symbol in a combined stream.
cfg->on_frame_filter = eph::net::make_twophase_filter(
    [](const uint8_t* data, size_t len) -> uint32_t {
        return extract_symbol_hash(data, len); // return 0 for unknown
    });
```

The two-phase filter uses an open-addressed 256-slot hash table on the
stack, runs in O(n) on batches of up to `kMaxFramesPerBatch = 128`
frames, and mutates the `deliver` flag on each `FrameView`. Control and
fragmented frames bypass filtering.

### Reconnection and subscription replay

```cpp
cfg->on_reconnect_attempt = [](int n, int max, std::string_view err) {
    spdlog::warn("reconnect {}/{}: {}", n, max, err);
    return true; // keep trying; return false to abort
};

cfg->on_reconnected = [&](int attempt, uint64_t downtime_ns, uint64_t total) {
    spdlog::info("reconnected (attempt {}, down {:.1f}ms, total {})",
                 attempt, downtime_ns / 1e6, total);
    // Replay subscriptions
    (void)transport->send_text(R"({"method":"SUBSCRIBE","params":[...]})");
};
```

`ReconnectPolicy` doubles the backoff after each failure (up to
`max_reconnect_backoff`, or 16x `reconnect_interval` if unset) and
applies ±25% jitter so reconnecting fleets don't synchronise.

## Public API surface (abbreviated)

See the Doxygen-style `///` comments on each header for the
authoritative contract. A high-level summary:

### `Transport<TcpImpl, Framer, MaxPayload, QueueDepth, RxQueueTmpl, LastOnlyDeliver>`

- `create(TcpFactory, TransportConfig) -> expected<unique_ptr, ConnectionErrorInfo>`
- Send: `send`, `send_binary`, `send_text`, `send_text_unchecked`,
  `send_for`, `send_text_for`, `send_binary_for`, `send_n`, `send_n_for`,
  `send_close`, `send_ping`
- Receive: `recv`, `try_recv`, `try_recv_msg`, `recv_peek`,
  `peek_recv_msg`, `recv_n`, `drain_recv`, `wait_recv`, `wait_recv_msg`
- Lifecycle: `close_gracefully`, `stop`, `start_threads`,
  `is_running`, `state`, `is_connected`, `reconnect_now`
- Queue occupancy: `tx_queue_size`, `rx_queue_size`,
  `tx_queue_fill_ratio`, `rx_queue_fill_ratio`, `tx_queue_hwm`,
  `rx_queue_hwm`
- Stats: `stats`, `tx_stats`, `rx_stats`, `reset_stats`,
  `connection_info`, `tls_version`, `cipher_name`, `remote_ip`
- Configuration: `config`, `max_payload`, `queue_depth`,
  `timestamps_enabled`

### `DirectTxTransport<...>`

Same API as `Transport<>`, minus TX-queue observability
(`tx_queue_*` methods), plus direct-TX semantics: `send()` is
synchronous — `kOk` means handed to the TCP layer, not merely
enqueued. `tx_queue_hwm` is always 0 in `TransportStats`.

### `DirectTransport<TcpImpl, Framer, MaxPayload>`

No threads, no queues. API is:

- `create`, `stop`, `is_running`, `state`, `is_connected`
- Send: `send`, `send_binary`, `send_text`, `send_text_unchecked`,
  `send_n`, `send_close`, `send_ping`
- Receive driver: `poll()` (all-in-one TCP poll + decrypt + decode) or
  `feed_rx()` + `process_pending()` (Reactor split)
- Diagnostics: `stats`, `rtt_stats`, `rx_latency_stats`,
  `rx_decrypt_stats`, `rx_decode_stats`,
  `rx_latency_histogram_snapshot`, `connection_info`, `reset_stats`

Data is delivered to the `on_message` callback configured on the
`TransportConfig`; there is no RX queue to poll.

### `TransportConfig`

All fields are documented inline in `transport_types.hpp`. Highlights:

- Connection target: `remote_host`, `remote_port`, `ws_path`,
  `ws_subprotocol`, `extra_headers`
- TLS: `use_tls`, `ca_cert_path`, `verify_peer`, `client_cert_path`,
  `client_key_path`, `pinned_spki_sha256`, `on_pin_mismatch`
- Timeouts: `tcp_timeout`, `tls_timeout`, `ws_timeout`
- Performance: `tx_burst_size`, `rx_burst_size`, `skip_utf8_validation`
- Reconnection: `reconnect_interval`, `max_reconnect_backoff`,
  `max_reconnect_attempts`
- WebSocket keepalive: `ping_interval`, `pong_timeout`
- CPU affinity: `tx_cpu`, `rx_cpu`
- Callbacks: `on_state_change`, `on_message`, `on_close`, `on_ping`,
  `on_pong`, `on_rx_drop`, `on_reconnect_attempt`, `on_reconnected`,
  `on_frame_filter`, `on_connected_before_threads`, `thresholds.on_breach`
- Utilities: `validate()`, `warnings()`, `dump()`, `to_json()`,
  `to_url()`, `from_url()`, `operator==`

### `ReconnectPolicy`

Standalone, testable exponential-backoff state machine:

- `ReconnectPolicy(const TransportConfig&)`
- `attempt(connect_fn) -> bool` — sleeps for current backoff with
  ±25% jitter, invokes `connect_fn`, updates state, fires
  `on_reconnect_attempt`
- `exhausted() -> bool`, `attempts() -> int`, `reset()`,
  `total_reconnects() -> uint64_t`

### WebSocket protocol (`ws::` namespace)

Defined in `detail/websocket.hpp`, also re-exported implicitly by
`Transport<>`:

- Opcodes: `ws::opcode::kText`, `kBinary`, `kClose`, `kPing`, `kPong`,
  `kContinuation`
- Close codes: `ws::close_code::kNormal`, `kGoingAway`,
  `kProtocolError`, …
- Encode/decode: `encode_frame`, `decode_frame`, `encode_frame_header`,
  `build_close_frame`, `build_ping_frame`, `build_pong_frame`
- Validation: `is_valid_utf8`, `is_valid_close_code`,
  `is_valid_payload_len`
- Masking: `apply_mask`, `masked_copy`, `generate_mask_key`,
  `MaskKeyCache`
- Sizes: `frame_header_size`, `total_frame_size`, `kMaxFrameHeaderLen`
- Naming: `opcode_name`, `close_code_name`
- Formatters: `Opcode`, `CloseCode` (for `std::format`)
- Template: `FrameTemplate` (precomputed header for hot-path encode)

### HTTP (`http::` namespace)

Minimal HTTP/1.1 **only** for WebSocket Upgrade:

- `http::generate_ws_key() -> expected<string, string>`
- `http::build_upgrade_request(host, path, key, extra) -> expected<string, string>`
- `http::parse_upgrade_response(data, len) -> expected<UpgradeResponse, string>`
- `http::validate_ws_accept(key, accept) -> bool` (SHA-1 + base64)

### TLS types

- `TlsSession<TcpImpl>` — TLS 1.3 handshake via aws-lc custom BIO
  bound to a `TcpTransport`. Exports session keys then steps off
  the data path.
- `TlsRecordCrypto` — composed `TlsEncryptor` + `TlsDecryptor`,
  thread-safe for split TX/RX ownership (TX thread writes, RX
  thread reads; no crossover).
- `TlsEncryptor` / `TlsDecryptor` — AES-128/256-GCM AEAD,
  direction-specific, each owns key/IV/sequence.
- `tls_record::build_nonce`, `write_record_header`,
  `parse_record_header`, `kMaxSequenceNumber` (2^24, NIST SP 800-38D —
  forces reconnection before key reuse).

## Observability

All non-trivial functions emit leveled `SPDLOG_LOGGER_*` output.
Filters apply at compile-time via `SPDLOG_ACTIVE_LEVEL` (propagated
from the top-level xmake option `net_log_level`). Log messages
include actionable context: remote host, attempt number, error
details, byte counts.

`TransportStats` aggregates per-stream counters, TLS sequence numbers,
queue HWMs, handshake timings (TCP / TLS / WS breakdown), RTT
statistics, and — when timestamps are enabled at compile time — HdrHistogram-
based latency distributions for TX (total, queue-wait, encode+encrypt)
and RX (total, decrypt, decode). Snapshots support `operator-` for
windowed metrics and `dump()` / `to_json()` for logging and monitoring
integration. Rate helpers: `tx_pps()`, `rx_pps()`, `tx_mbps()`,
`rx_mbps()`.

## Tests and benchmarks

Unit and integration tests live under `tests/`:

```
tests/
├── test_framers.cpp              # WsFramer / RawFramer contracts
├── test_http.cpp                 # HTTP/1.1 upgrade encode/parse
├── test_reconnect_policy.cpp     # Exponential backoff + jitter bounds
├── test_tls_config.cpp           # TlsConfig validation and warnings
├── test_tls_record.cpp           # AEAD roundtrip, record header, seq limits
├── test_transport_config.cpp     # TransportConfig validate/dump/to_url/from_url
├── test_transport_types.cpp      # Stats delta, ConnectionInfo, formatters
└── test_websocket.cpp            # Encode/decode, close handshake, masking
```

Benchmarks (`benchmarks/bench_transport_types.cpp`) cover the hot-path
types: `encode_frame` (64B/512B), `apply_mask`, AES-GCM encrypt/decrypt,
HKDF, stats `dump`/`to_json`, `from_url`, and TLS record parse.

Fuzzing (`fuzzers/fuzz_ws_decode.cpp`) targets `ws::decode_frame` with
libFuzzer.

```bash
xmake build -g tests && xmake run -g tests
xmake build -g benchmarks && xmake run bench_transport_types
```

## License

Part of the `ephemeral_dev` monorepo. See the top-level `LICENSE`
file for licensing terms.
