# eph-transport

Header-only C++23 library for low-latency WebSocket and raw TCP transport with TLS 1.3 encryption, designed for HFT market data feeds. Provides three transport variants with different threading models, all sharing the same connection lifecycle, TLS crypto, and WebSocket protocol implementation.

## Overview

eph-transport is the network transport layer of the eph ecosystem. It sits between a TCP backend (e.g., `eph-net` kernel sockets, `eph-dpdk` DPDK poll-mode) and application-level protocol parsers (e.g., `eph-fix`, `eph-itch`, `eph-json`). Its job is to establish encrypted WebSocket connections, manage connection lifecycle (handshake, reconnection, graceful close), and move data between the wire and the application with minimal latency.

The library is generic over the TCP backend via the `TcpTransport` concept (defined in `eph-core`) and over the wire framing via the `MessageFramer` concept, making it usable with any socket implementation or custom protocol framer.

### Architecture

```
Application thread          TX thread              Network
     |                         |                      |
     |-- send() --> [TX SPSC] --> frame+encrypt+send --|
     |                                                 |
     |-- recv() <-- [RX SPSC] <-- recv+decrypt+parse --|
     |                         |                      |
                            RX thread
```

Three transport variants address different latency/complexity tradeoffs:

| Variant | TX Path | RX Path | Use Case |
|---------|---------|---------|----------|
| `Transport` | TX thread + SPSC queue | RX thread + SPSC queue | General-purpose, non-blocking send/recv |
| `DirectTxTransport` | Direct from app thread | RX thread + SPSC queue | Lowest TX latency, async receive |
| `DirectTransport` | Direct from app thread | Direct from app thread | Single-threaded event loops (Reactor, io_uring, DPDK) |

## Key Components

All headers are under `include/eph/transport/`:

### Transport Variants

- **transport.hpp** -- Threaded transport with dedicated TX and RX threads connected via lock-free SPSC queues. Application thread calls non-blocking `send()`/`recv()`. Supports push-mode delivery via `on_message` callback, batch frame filtering for multi-symbol streams, and automatic reconnection with exponential backoff. Template parameters control `TcpImpl`, `Framer`, `MaxPayload`, `QueueDepth`, `RxQueueTmpl`, and `LastOnlyDeliver`.
- **direct_transport.hpp** -- Threadless, queueless transport for single-threaded event loops (Reactor, io_uring, DPDK poll-mode). Application thread does everything: `send()` for TX, `feed_rx()`/`process_pending()`/`poll()` for RX. Minimal latency path with no SPSC overhead.
- **direct_tx_transport.hpp** -- Hybrid: direct TX from the application thread (no TX queue), background RX thread with RX queue. Eliminates TX queue latency while keeping asynchronous receive.

### Presets

- **presets.hpp** -- Canonical type aliases combining payload size, queue depth, and framer. `DefaultTransport` (512B/1024), `SmallTransport` (64B/256), `LargeTransport` (4096B/512), `EvictTransport` (evicting RX queue), `RawTransport` (no WebSocket framing). Matching `DirectTx*` and `Direct*` variants for each.

### Configuration and Types

- **transport_types.hpp** -- `TransportConfig` (connection target, TLS settings, timeouts, reconnect policy, CPU affinity, callbacks), `TransportEvent`/`TransportState` enums, `TransportStats`/`RttStats` aggregate snapshots, `FrameView`/`FrameFilterFn` for batch frame filtering, and `make_twophase_filter()` for latest-per-symbol deduplication using a two-phase hash scan.
- **reconnect_policy.hpp** -- `ReconnectPolicy` implementing exponential backoff with +/-25% jitter. Independent, testable component that tracks attempt count and backoff state.

### Internal Workers

- **tx_worker.hpp** -- `TxWorker`: owns the TX thread, TX SPSC queue, ping/pong scheduling, TLS sequence monitoring, and TX stats/histograms. Busy-polls the queue, builds WS frames, encrypts via AEAD, and sends over TCP.
- **rx_worker.hpp** -- `RxWorker`: owns the RX thread, RX SPSC queue, and RX stats/histograms. Busy-polls TCP, reassembles TLS records, decrypts, parses WS frames via `FrameProcessor`, and pushes to the RX queue or invokes `on_message`.
- **transport_core.hpp** -- `TransportCore<TcpImpl>`: shared connection state (TCP socket, TLS crypto, config, lifecycle atomics, connection metadata). Implements `do_connect()` (TCP + TLS 1.3 handshake + key export) and `do_ws_upgrade()` (RFC 6455 HTTP Upgrade).
- **frame_processor.hpp** -- `FrameProcessor`: decodes WS/generic frames, handles fragmentation reassembly, delivers data frames via a `DeliverPolicy`, and sends control frame responses (pong/close) via a `SendFn`.

### Protocol Implementations

- **websocket.hpp** -- RFC 6455 WebSocket protocol: frame encoding/decoding with client masking, ping/pong, close handshake, frame header precomputation. Opcodes, close codes, and `opcode_name()` utility.
- **ws_framer.hpp** -- `WsFramer`: adapts `ws::encode_frame`/`ws::decode_frame` to the `MessageFramer` concept for use with Transport.
- **raw_framer.hpp** -- `RawFramer`: pass-through framer with zero overhead. For protocols that handle their own message boundaries (e.g., FIX).
- **http.hpp** -- Minimal HTTP/1.1 for WebSocket Upgrade only: `build_upgrade_request()`, `parse_upgrade_response()`, `validate_ws_accept()`, `generate_ws_key()`. Not a general-purpose HTTP library.

### TLS 1.3 Stack

- **tls_session.hpp** -- `TlsSession<TcpImpl>`: TLS 1.3 handshake via aws-lc (BoringSSL-compatible) with custom BIO backed by any `TcpTransport`. Extracts session keys for hot-path AEAD; does NOT do data-plane I/O.
- **tls_record.hpp** -- `TlsRecordCrypto`: backward-compatible composition of `TlsEncryptor` + `TlsDecryptor`. Thread-safe for split TX/RX thread ownership.
- **tls_encryptor.hpp** -- `TlsEncryptor`: AES-128/256-GCM record-level encryption (write direction). Owns AEAD context, write IV, and sequence number.
- **tls_decryptor.hpp** -- `TlsDecryptor`: AES-128/256-GCM record-level decryption (read direction). Owns AEAD context, read IV, and sequence number.
- **tls_constants.hpp** -- TLS record constants, nonce construction (`build_nonce`), header read/write, sequence number limits (2^24 per NIST SP 800-38D with forced reconnection).

### Internal

- **detail/message_types.hpp** -- Cache-line-aligned `TxMessage`/`RxMessage` structs for SPSC queues, with optional TSC timestamps.

## Public API Reference

### Transport Class (threaded)

```cpp
template <TcpTransport TcpImpl, MessageFramer Framer = WsFramer,
          size_t MaxPayload = 512, size_t QueueDepth = 1024,
          template <typename, size_t> class RxQueueTmpl = BoundedQueue,
          bool LastOnlyDeliver = false>
class Transport;
```

| Method | Description |
|--------|-------------|
| `static create(TcpFactory, TransportConfig) -> expected<unique_ptr<Transport>, ConnectionErrorInfo>` | Factory: blocking TCP + TLS + WS handshake, returns owning pointer |
| `send(data, len, opcode) -> SendError` | Non-blocking enqueue to TX thread (binary frame by default) |
| `send(span<uint8_t>, opcode) -> SendError` | Span overload of send |
| `send_binary(data, len) -> SendError` | Explicit binary frame send |
| `send_text(data, len) -> SendError` | Text frame with UTF-8 validation |
| `send_text(string_view) -> SendError` | Text frame from string_view |
| `send_text_unchecked(data, len) -> SendError` | Text frame without UTF-8 validation (hot path) |
| `send_for(data, len, timeout, opcode) -> SendError` | Send with backpressure wait |
| `send_n(payloads, count, opcode) -> SendError` | Batch send (all-or-nothing, amortized atomics) |
| `send_close(status_code, reason) -> SendError` | Send WebSocket Close frame |
| `send_ping(payload, len) -> SendError` | Send WebSocket Ping frame |
| `recv(callback) -> bool` | Non-blocking poll: `(data*, len)`, `(data*, len, opcode)`, or `(data*, len, opcode, tsc)` |
| `try_recv() -> optional<vector<uint8_t>>` | Copy-out receive (convenience) |
| `try_recv_msg() -> optional<ReceivedMessage>` | Receive with opcode metadata |
| `recv_peek(callback) -> bool` | Peek without consuming |
| `recv_n(callback, max_count) -> size_t` | Batch receive (amortized atomics) |
| `drain_recv(callback) -> size_t` | Drain all available messages |
| `wait_recv(callback, timeout) -> bool` | Blocking receive with timeout |
| `close_gracefully(code, reason, timeout) -> bool` | RFC 6455 graceful close handshake |
| `stop()` | Immediate shutdown (sends Close, joins threads) |
| `start_threads()` | Start workers (only when `deferred_start=true`) |
| `is_running() -> bool` | Check if transport is operational |
| `state() -> TransportState` | Current state: Connected / Reconnecting / Stopped |
| `is_connected() -> bool` | Shorthand for `state() == kConnected` |
| `reconnect_now() -> bool` | Force immediate reconnection |
| `stats() -> TransportStats` | Aggregated TX/RX/TLS/RTT statistics snapshot |
| `tx_stats() -> TxWorkerStats` | TX-only statistics |
| `rx_stats() -> RxWorkerStats` | RX-only statistics |
| `connection_info() -> ConnectionInfo` | TLS version, cipher, IP, subprotocol |
| `tx_queue_size() -> size_t` | Approximate TX queue occupancy |
| `rx_queue_size() -> size_t` | Approximate RX queue occupancy |
| `tx_queue_fill_ratio() -> double` | TX queue fill [0.0, 1.0] |
| `rx_queue_fill_ratio() -> double` | RX queue fill [0.0, 1.0] |
| `config() -> const TransportConfig&` | Read-only access to configuration |

### DirectTransport Class (threadless)

```cpp
template <TcpTransport TcpImpl, MessageFramer Framer = WsFramer,
          size_t MaxPayload = 512>
class DirectTransport;
```

| Method | Description |
|--------|-------------|
| `static create(TcpFactory, TransportConfig) -> expected<unique_ptr, ConnectionErrorInfo>` | Factory (no threads started) |
| `send(data, len, opcode) -> SendError` | Synchronous: frame + encrypt + TCP send on calling thread |
| `poll()` | TCP poll + decrypt + decode (all-in-one for simple loops) |
| `feed_rx(data, len)` | Feed raw TCP bytes for Reactor integration |
| `process_pending()` | Decrypt + decode buffered data |
| `stop()` | Shutdown (no threads to join) |

### DirectTxTransport Class (hybrid)

```cpp
template <TcpTransport TcpImpl, MessageFramer Framer = WsFramer,
          size_t MaxPayload = 512, size_t QueueDepth = 1024,
          template <typename, size_t> class RxQueueTmpl = BoundedQueue,
          bool LastOnlyDeliver = false>
class DirectTxTransport;
```

| Method | Description |
|--------|-------------|
| `static create(TcpFactory, TransportConfig) -> expected<unique_ptr, ConnectionErrorInfo>` | Factory: blocking handshake, starts RX thread |
| `send(data, len, opcode) -> SendError` | Synchronous send from app thread (no TX queue) |
| `recv(callback) -> bool` | Non-blocking poll from RX SPSC queue |
| `stop()` | Shutdown (joins RX thread) |

### Preset Aliases

All presets are parameterized on `TcpImpl`:

| Alias | Base Class | Payload | Queue | Framer |
|-------|-----------|---------|-------|--------|
| `DefaultTransport<T>` | `Transport` | 512B | 1024 | WsFramer |
| `SmallTransport<T>` | `Transport` | 64B | 256 | WsFramer |
| `LargeTransport<T>` | `Transport` | 4096B | 512 | WsFramer |
| `EvictTransport<T>` | `Transport` | 512B | 1024 | WsFramer (EvictingQueue) |
| `RawTransport<T>` | `Transport` | 512B | 1024 | RawFramer |
| `DirectTxDefaultTransport<T>` | `DirectTxTransport` | 512B | 1024 | WsFramer |
| `DirectTxSmallTransport<T>` | `DirectTxTransport` | 64B | 256 | WsFramer |
| `DirectTxRawTransport<T>` | `DirectTxTransport` | 512B | 1024 | RawFramer |
| `DirectDefaultTransport<T>` | `DirectTransport` | 512B | -- | WsFramer |
| `DirectSmallTransport<T>` | `DirectTransport` | 64B | -- | WsFramer |
| `DirectRawTransport<T>` | `DirectTransport` | 512B | -- | RawFramer |

### TransportConfig

```cpp
struct TransportConfig {
    // Connection target
    std::string remote_host;              // Hostname (TLS SNI + HTTP Host)
    uint16_t    remote_port = 443;        // TCP port
    std::string ws_path     = "/";        // WebSocket upgrade URI path
    std::string ws_subprotocol;           // Sec-WebSocket-Protocol header
    std::string extra_headers;            // Additional HTTP headers (must end with \r\n)

    // TLS
    bool        use_tls     = true;       // Enable TLS 1.3
    std::string ca_cert_path;             // CA cert (PEM), empty = system default
    bool        verify_peer = true;       // Verify server certificate
    std::string client_cert_path;         // mTLS client cert (PEM)
    std::string client_key_path;          // mTLS client key (PEM)

    // Timeouts
    milliseconds tcp_timeout{3000};
    milliseconds tls_timeout{5000};
    milliseconds ws_timeout{3000};

    // Performance tuning
    uint16_t tx_burst_size = 32;          // Max messages drained per TX loop
    uint16_t rx_burst_size = 32;          // Max TCP segments polled per RX loop
    bool skip_utf8_validation = true;     // Skip UTF-8 check on text frames

    // Reconnection (exponential backoff with jitter)
    milliseconds reconnect_interval{100}; // Base backoff
    milliseconds max_reconnect_backoff{0};// Cap (0 = 16x base)
    int max_reconnect_attempts = 10;      // 0 = disable auto-reconnect

    // WebSocket keepalive
    seconds ping_interval{30};            // 0 = disable
    seconds pong_timeout{0};              // 0 = disable pong timeout

    // CPU affinity
    int tx_cpu = -1;                      // -1 = no pinning
    int rx_cpu = -1;

    // Callbacks (all called from worker threads, must be non-blocking)
    TransportStateCallback on_state_change;
    function<void(const uint8_t*, uint16_t, uint8_t)> on_message;
    function<void(uint16_t code, string_view reason)> on_close;
    function<void(const uint8_t*, uint16_t)> on_ping;
    function<void(const uint8_t*, uint16_t)> on_pong;
    function<void(uint64_t total_dropped)> on_rx_drop;
    function<bool(int attempt, int max, string_view err)> on_reconnect_attempt;
    function<void(int attempt, uint64_t downtime_ns, uint64_t total)> on_reconnected;
    FrameFilterFn on_frame_filter;        // Batch frame filter for symbol dedup
    function<void()> on_connected_before_threads;
    bool deferred_start = false;          // Don't start threads in create()

    // Utility
    string_view validate() const;              // Early validation (empty = OK)
    vector<string> warnings() const;           // Non-fatal advisories
    string dump() const;                       // Multi-line formatted dump
    string to_json() const;                    // JSON serialization
    string to_url() const;                     // Reconstruct ws(s):// URL
    static expected<TransportConfig, string> from_url(string_view);  // Parse ws(s):// URL
};
```

### Enums

| Enum | Values | Description |
|------|--------|-------------|
| `TransportEvent` | `kConnected`, `kDisconnected`, `kReconnecting`, `kStopped` | Lifecycle events for `on_state_change` |
| `TransportState` | `kConnected`, `kReconnecting`, `kStopped` | Pollable connection state |
| `SendError` | `kOk`, `kNotConnected`, `kQueueFull`, `kMessageTooLarge`, `kNullData`, `kInvalidUtf8`, `kInvalidCloseCode` | Send result codes |
| `ConnectionError` | `kInvalidConfig`, `kFactoryFailed`, `kTlsHandshakeFailed`, `kWsUpgradeRejected`, ... | Connection failure categories |
| `ws::DecodeError` | `kIncomplete`, `kReservedBits`, `kFragmentedControl`, `kControlPayloadTooLarge`, `kInvalidOpcode` | Frame decode errors |

### Statistics Types

| Type | Description |
|------|-------------|
| `TransportStats` | Aggregated snapshot: TX/RX packets/bytes/drops, TLS seq numbers, queue HWMs, handshake timings, RTT, uptime. Supports `operator-` for windowed metrics and `dump()`/`to_json()` for serialization. Rate helpers: `tx_pps()`, `rx_pps()`, `tx_mbps()`, `rx_mbps()`. |
| `TxWorkerStats` | TX-specific: packets, bytes, drops, crypto errors, queue HWM, latency histograms (total TX, queue wait, encode+encrypt). |
| `RxWorkerStats` | RX-specific: packets, bytes, drops, decrypt errors, WS pings/pongs, queue HWM, latency histograms (total RX, decrypt, decode). |
| `RttStats` | Ping/pong round-trip: count, min, max, mean, p50, p99, p999 in nanoseconds. Convenience `*_us()` methods. |
| `ConnectionInfo` | TLS version, cipher suite, WebSocket subprotocol, remote IP. `dump()` and `to_json()`. |

### WebSocket Protocol (`ws::` namespace)

| Symbol | Description |
|--------|-------------|
| `ws::opcode::kText`, `kBinary`, `kClose`, `kPing`, `kPong`, `kContinuation` | RFC 6455 frame opcodes |
| `ws::close_code::kNormal`, `kGoingAway`, `kProtocolError`, ... | RFC 6455 close status codes |
| `ws::encode_frame(out, opcode, payload, len) -> size_t` | Encode a masked WebSocket frame |
| `ws::decode_frame(data, len) -> expected<DecodedFrame, DecodeError>` | Decode a WebSocket frame |
| `ws::encode_frame_header(out, opcode, len, fin, mask) -> size_t` | Encode frame header only |
| `ws::build_close_frame(out, code, reason) -> size_t` | Build a Close frame payload |
| `ws::opcode_name(uint8_t) -> string_view` | Human-readable opcode name |
| `ws::close_code_name(uint16_t) -> string_view` | Human-readable close code name |
| `ws::is_valid_close_code(uint16_t) -> bool` | Validate close code for sending |
| `ws::is_valid_utf8(data, len) -> bool` | DFA-based UTF-8 validation |
| `ws::apply_mask(data, len, mask)` | In-place XOR masking |
| `ws::masked_copy(dst, src, len, mask)` | Fused copy + mask (64-bit blocks) |
| `ws::frame_header_size(payload_len) -> size_t` | Compute header size for payload |
| `ws::total_frame_size(payload_len) -> size_t` | Header + payload size |
| `MaskKeyCache` | CSPRNG batch-pregenerated mask key pool (1024 keys, ~2ns/key) |

### HTTP (`http::` namespace)

| Function | Description |
|----------|-------------|
| `http::generate_ws_key() -> expected<string, string>` | Random 16-byte base64 WebSocket key |
| `http::build_upgrade_request(host, path, key, extra) -> expected<string, string>` | Build HTTP/1.1 Upgrade request |
| `http::parse_upgrade_response(data, len) -> expected<UpgradeResponse, string>` | Parse 101 Switching Protocols |
| `http::validate_ws_accept(key, accept) -> bool` | Validate Sec-WebSocket-Accept (SHA-1 + base64) |

### TLS Types

| Type | Description |
|------|-------------|
| `TlsSession<TcpImpl>` | TLS 1.3 handshake + key export via aws-lc custom BIO. Not used on data path. |
| `TlsRecordCrypto` | Composed `TlsEncryptor` + `TlsDecryptor`. Thread-safe for split TX/RX ownership. |
| `TlsEncryptor` | AES-GCM write-direction AEAD. `create(state, key_len)`, `encrypt(plaintext, len, out)`. |
| `TlsDecryptor` | AES-GCM read-direction AEAD. `create(state, key_len)`, `decrypt(record, len, out, out_len)`. |
| `TlsHotState` | 4-cache-line key material: write key/IV/seq + read key/IV/seq. Scrubbed on destruction. |
| `TlsKeyMaterial` | Per-direction: key+IV (cache line 1, read-only) + seq number (cache line 2, hot write). |
| `tls_record::build_nonce(out, iv, seq)` | RFC 8446 nonce = IV XOR big-endian seq |
| `tls_record::write_record_header(dst, type, len)` | Write 5-byte TLS record header |
| `tls_record::parse_record_header(src, type, len) -> bool` | Parse and validate TLS record header |
| `tls_record::kMaxSequenceNumber` | 2^24 records before forced reconnect (NIST SP 800-38D) |

### Frame Filter

| Symbol | Description |
|--------|-------------|
| `FrameView` | Lightweight view of a decoded frame: payload, len, opcode, deliver flag |
| `FrameFilterFn` | `function<void(span<FrameView>)>` batch filter callback |
| `make_twophase_filter(extractor) -> FrameFilterFn` | Two-phase forward scan: keeps only latest frame per symbol hash |

### Reconnect Policy

| Method | Description |
|--------|-------------|
| `ReconnectPolicy(config)` | Construct from TransportConfig |
| `attempt(connect_fn) -> bool` | Sleep (backoff + jitter), call connect_fn, update state |
| `exhausted() -> bool` | True if max attempts reached |
| `reset()` | Reset after successful connection |
| `attempts() -> int` | Current attempt count |
| `total_reconnects() -> uint64_t` | Lifetime successful reconnection count |

## Dependencies

- **eph-core** -- `TcpTransport` concept (`tcp_concept.hpp`), `MessageFramer` concept (`framer_concept.hpp`), transport error types (`transport_errors.hpp`), JSON escape utility
- **eph-containers** -- `BoundedQueue` and `EvictingQueue` (lock-free SPSC queues)
- **eph-utils** -- HDR histogram, TSC/time utilities, CPU affinity, cache-line alignment
- **spdlog** -- Structured logging throughout
- **OpenSSL / aws-lc** -- TLS 1.3 handshake (libssl) and AES-GCM AEAD encryption (libcrypto)

## Usage Examples

### Threaded Transport (default)

```cpp
#include <eph/transport/transport.hpp>
#include <eph/transport/presets.hpp>

// TransportConfig from a WebSocket URL
auto cfg = eph::net::TransportConfig::from_url(
    "wss://stream.binance.com:9443/ws/btcusdt@bookTicker");
assert(cfg);
cfg->on_message = [](const uint8_t* data, uint16_t len, uint8_t opcode) {
    // Push-mode: called directly from RX thread -- must be non-blocking
    process_market_data(data, len);
};

// Factory creates a fresh TCP socket on each (re)connect
auto factory = [&]() -> std::expected<std::unique_ptr<MyTcp>, std::string> {
    auto tcp = std::make_unique<MyTcp>();
    if (auto r = tcp->connect(std::chrono::milliseconds{3000}); !r)
        return std::unexpected(r.error());
    return tcp;
};

// Blocking handshake: TCP + TLS 1.3 + WebSocket Upgrade
auto result = eph::net::DefaultTransport<MyTcp>::create(
    std::move(factory), *cfg);
if (!result) {
    spdlog::error("Connect failed: {}", result.error().message());
    return 1;
}
auto& transport = *result;

// Non-blocking send (enqueues to TX thread)
auto err = transport->send(R"({"method":"SUBSCRIBE"})", 23);

// Pull-mode receive (alternative to on_message callback)
transport->recv([](const uint8_t* data, size_t len, uint8_t opcode) {
    // data is valid only during this callback
});

// Windowed statistics
auto s1 = transport->stats();
std::this_thread::sleep_for(std::chrono::seconds{10});
auto delta = transport->stats() - s1;
spdlog::info("RX rate: {:.0f} msg/s, p99 RTT: {:.1f}us",
    delta.rx_pps(), delta.rtt.p99_us());

// Graceful shutdown
transport->close_gracefully();
```

### Direct Transport (threadless)

```cpp
#include <eph/transport/direct_transport.hpp>

auto result = eph::net::DirectTransport<MyTcp>::create(
    std::move(factory), config);
auto& dt = *result;

// Application thread does everything
dt->send(data, len);          // frame + encrypt + TCP send
dt->poll();                   // TCP poll + decrypt + decode

// Or split for Reactor integration:
dt->feed_rx(raw_data, len);   // feed raw TCP bytes
dt->process_pending();        // decrypt + decode buffered data
```

### DirectTx Transport (hybrid)

```cpp
#include <eph/transport/presets.hpp>

auto result = eph::net::DirectTxDefaultTransport<MyTcp>::create(
    std::move(factory), config);
auto& dtx = *result;

// TX is synchronous on app thread (lowest TX latency)
dtx->send(order_bytes.data(), order_bytes.size());

// RX is async via background thread + SPSC queue
dtx->recv([](const uint8_t* data, size_t len, uint8_t opcode) {
    handle_response(data, len);
});
```

### Evicting Transport (latest-value semantics)

```cpp
#include <eph/transport/presets.hpp>

// EvictTransport overwrites oldest unread message on RX overflow
// Ideal for market data where only the latest quote matters
auto result = eph::net::EvictTransport<MyTcp>::create(
    std::move(factory), config);
```

### Batch Frame Filter (multi-symbol dedup)

```cpp
// Keep only the latest message per symbol in combined streams
config.on_frame_filter = eph::net::make_twophase_filter(
    [](const uint8_t* data, size_t len) -> uint32_t {
        // Extract a symbol hash from the JSON payload
        // Return 0 for unrecognized payloads (always delivered)
        return extract_symbol_hash(data, len);
    });
```

### URL Parsing

```cpp
auto cfg = eph::net::TransportConfig::from_url("wss://api.example.com:8443/v1/ws?token=abc");
// cfg->remote_host = "api.example.com"
// cfg->remote_port = 8443
// cfg->ws_path     = "/v1/ws?token=abc"
// cfg->use_tls     = true
```
