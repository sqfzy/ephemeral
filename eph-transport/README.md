# eph-transport

Header-only C++23 library for low-latency WebSocket and raw TCP transport with TLS 1.3 encryption, designed for HFT market data feeds. Provides three transport variants with different threading models, all sharing the same connection lifecycle, TLS crypto, and WebSocket protocol implementation.

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

## Dependencies

- **eph-core** -- `TcpTransport` concept (`tcp_concept.hpp`), `MessageFramer` concept (`framer_concept.hpp`), transport error types (`transport_errors.hpp`), JSON escape utility
- **eph-containers** -- `BoundedQueue` and `EvictingQueue` (lock-free SPSC queues)
- **eph-utils** -- HDR histogram, TSC/time utilities, CPU affinity, cache-line alignment
- **spdlog** -- Structured logging throughout
- **OpenSSL / aws-lc** -- TLS 1.3 handshake (libssl) and AES-GCM AEAD encryption (libcrypto)

## Quick Start

```cpp
#include <eph/transport/transport.hpp>
#include <eph/transport/presets.hpp>

// Define your TCP backend (must satisfy TcpTransport concept)
// e.g., eph::net::TcpSession from eph-net

TransportConfig config{
    .remote_host = "stream.binance.com",
    .remote_port = 9443,
    .ws_path     = "/ws/btcusdt@bookTicker",
    .use_tls     = true,
};

// Factory creates a connected TCP socket on each (re)connect
auto factory = [&]() -> std::expected<std::unique_ptr<MyTcp>, std::string> {
    auto tcp = std::make_unique<MyTcp>(/* ... */);
    auto r = tcp->connect(std::chrono::milliseconds{3000});
    if (!r) return std::unexpected(r.error());
    return tcp;
};

// Create transport (blocking: TCP + TLS + WS handshake)
auto result = eph::net::DefaultTransport<MyTcp>::create(std::move(factory), config);
if (!result) { /* handle result.error() */ }
auto& transport = *result;

// Send (non-blocking, enqueues to TX thread)
transport->send(data, len);

// Receive (poll RX queue)
transport->recv([](const uint8_t* data, uint16_t len, uint8_t opcode) {
    // process message
});

// Or use push-mode delivery (set config.on_message before create)
config.on_message = [](const uint8_t* data, uint16_t len, uint8_t opcode) {
    // called directly from RX thread — must be non-blocking
};

// Graceful shutdown
transport->close_gracefully();
```

### Direct Transport (threadless)

```cpp
#include <eph/transport/direct_transport.hpp>

auto result = eph::net::DirectTransport<MyTcp>::create(std::move(factory), config);
auto& dt = *result;

// Application thread does everything
dt->send(data, len);          // encode + encrypt + TCP send
dt->poll();                   // TCP poll + decrypt + decode
// Or split for Reactor integration:
dt->feed_rx(raw_data, len);   // feed raw TCP bytes
dt->process_pending();        // decrypt + decode buffered data
```
