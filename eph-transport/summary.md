# eph-transport Summary

## Overview

eph-transport is the network transport layer of the eph high-frequency trading library ecosystem. It provides encrypted WebSocket and raw TCP transport with TLS 1.3 encryption, designed for sub-microsecond-class market data feeds. The library sits between a TCP backend (kernel sockets via eph-net, DPDK poll-mode via eph-dpdk) and application-level protocol parsers (eph-fix, eph-itch, eph-json), managing the full connection lifecycle from TCP handshake through TLS key export to WebSocket framing.

The library is generic over two axes: the TCP backend (via the `TcpTransport` concept from eph-core) and the wire framing (via the `MessageFramer` concept). This allows the same transport code to run atop kernel sockets, DPDK, or any future backend without modification. Three transport variants address different latency/threading tradeoffs: a fully threaded variant with SPSC queues for decoupled send/recv, a hybrid with direct TX and threaded RX, and a fully threadless variant for single-threaded event loops.

All headers are header-only C++23, leveraging `std::expected` for error handling, concepts for compile-time interface enforcement, and `constexpr`/`consteval` where possible. The TLS stack uses aws-lc (BoringSSL-compatible) for the handshake and exports session keys for a custom AEAD hot path that bypasses SSL_read/SSL_write entirely, minimizing data-plane latency.

The library includes comprehensive observability: per-message TSC timestamps (compile-time opt-in), HdrHistogram-based latency breakdowns for every pipeline stage (TX queue wait, encode, decrypt, decode), round-trip time measurement via WebSocket ping/pong, and aggregated statistics with JSON serialization for monitoring integration.

## Architecture

```
+---------------------------------------------------------------+
|                     Application Thread                        |
|  send()  send_text()  recv()  try_recv_msg()  stats()        |
+-----+---------------------+----------------------------------+
      |                     |
      v                     v
+-------------+     +-------------+     +-------------------+
|  TX SPSC    |     |  RX SPSC    |     | TransportConfig   |
|  Queue      |     |  Queue      |     | ReconnectPolicy   |
+------+------+     +------+------+     +-------------------+
       |                   ^
       v                   |
+------+------+     +------+------+     +-------------------+
|  TxWorker   |     |  RxWorker   |     | TransportCore     |
|  TX Thread  |     |  RX Thread  |     |  TCP + TLS state  |
+------+------+     +------+------+     |  lifecycle atoms  |
       |                   ^            +--------+----------+
       v                   |                     |
+------+-------------------+---------------------+----------+
|                    Wire Pipeline                           |
|  WsFramer/RawFramer  |  TlsEncryptor  |  TlsDecryptor    |
|  FrameProcessor      |  TlsRecordCrypto                  |
+------+-------------------+--------------------------------+
       |                   ^
       v                   |
+------+-------------------+--------------------------------+
|              TcpTransport (concept)                        |
|  eph-net: SocketSession  |  eph-dpdk: DpdkSession         |
+-----------------------------------------------------------+
```

## Module Map

| File | Responsibility | Key Types | Depends On |
|------|---------------|-----------|------------|
| `transport.hpp` | Threaded transport (TX+RX threads, SPSC queues) | `Transport<TcpImpl, Framer, ...>` | transport_core, tx_worker, rx_worker, reconnect_policy |
| `direct_transport.hpp` | Threadless transport (app thread does all I/O) | `DirectTransport<TcpImpl, Framer, MaxPayload>` | transport_core, frame_processor, reconnect_policy, tls_encryptor, tls_decryptor |
| `direct_tx_transport.hpp` | Hybrid: direct TX, threaded RX | `DirectTxTransport<TcpImpl, ...>` | transport_core, rx_worker, reconnect_policy, tls_encryptor |
| `transport_core.hpp` | Shared connection state, handshake logic | `TransportCore<TcpImpl>` | tls_session, tls_record, http, websocket, transport_types |
| `transport_types.hpp` | Public types: config, enums, stats, formatters | `TransportConfig`, `TransportStats`, `RttStats`, `TransportEvent`, `TransportState`, `FrameView`, `FrameFilterFn`, `ThreadStats`, `ConnectionInfo` | eph-core/transport_errors |
| `tx_worker.hpp` | TX thread, SPSC queue, ping scheduling, stats | `TxWorker<...>`, `TxWorkerStats` | transport_core, transport_types, tls_record, websocket |
| `rx_worker.hpp` | RX thread, SPSC queue, TLS reassembly, stats | `RxWorker<...>`, `RxWorkerStats`, `ReceivedMessage` | transport_core, frame_processor, tls_record, tls_constants |
| `frame_processor.hpp` | WS/generic frame decode, fragmentation, control frames | `FrameProcessor<TcpImpl, Framer, DeliverPolicy, SendFn, ...>` | transport_core, websocket, transport_types |
| `reconnect_policy.hpp` | Exponential backoff with jitter | `ReconnectPolicy` | transport_types |
| `websocket.hpp` | RFC 6455 wire format: encode/decode, masking, UTF-8 | `ws::DecodedFrame`, `ws::FrameTemplate`, `ws::MaskKeyCache`, `ws::DecodeError` | openssl/rand |
| `ws_framer.hpp` | MessageFramer adapter for WebSocket | `WsFramer` | websocket, framer_concept |
| `raw_framer.hpp` | Pass-through framer (no overhead) | `RawFramer` | framer_concept |
| `http.hpp` | HTTP/1.1 WebSocket Upgrade only | `http::UpgradeResponse`, `http::generate_ws_key()`, `http::build_upgrade_request()`, `http::parse_upgrade_response()`, `http::validate_ws_accept()` | openssl/evp, openssl/rand |
| `tls_session.hpp` | TLS 1.3 handshake + key export via custom BIO | `TlsSession<TcpImpl>`, `TlsConfig`, `TlsHotState`, `TlsKeyMaterial`, `TlsKeyIv` | openssl/ssl, openssl/hkdf, tcp_concept |
| `tls_record.hpp` | Backward-compatible TlsEncryptor + TlsDecryptor | `TlsRecordCrypto` | tls_encryptor, tls_decryptor, tls_constants |
| `tls_encryptor.hpp` | AES-GCM write-direction AEAD | `TlsEncryptor` | tls_constants, openssl/aead |
| `tls_decryptor.hpp` | AES-GCM read-direction AEAD | `TlsDecryptor` | tls_constants, openssl/aead |
| `tls_constants.hpp` | TLS record constants, nonce, header parse/write | `tls_record::*` constants and functions | tls_session |
| `presets.hpp` | Canonical type aliases | `DefaultTransport<T>`, `SmallTransport<T>`, `EvictTransport<T>`, `DirectTx*`, `Direct*` | transport, direct_transport, direct_tx_transport |
| `detail/message_types.hpp` | SPSC queue message structs, shared logger | `detail::TxMessage<N>`, `detail::RxMessage<N>`, `detail::transport_logger()` | websocket, tls_record, alignment |

## Data Flow

### TX Path (Threaded Transport)

```
App Thread                 TX Thread              Network
    |                          |                      |
    |  send(data, len)         |                      |
    |  -> UTF-8 check (text)   |                      |
    |  -> memcpy to TxMsg      |                      |
    |  -> SPSC try_produce     |                      |
    |          |               |                      |
    |          +-- [TxMsg] --> try_consume_n (batch)   |
    |                          |                      |
    |                   WS frame encode               |
    |                   (WsFramer or RawFramer)       |
    |                          |                      |
    |                   [TLS encrypt]                  |
    |                   (AEAD seal + record header)    |
    |                          |                      |
    |                   TCP send (coalesced batch) --->|
    |                          |                      |
    |                   stats: packets, bytes, latency |
```

### RX Path (Threaded Transport)

```
Network                    RX Thread              App Thread
    |                          |                      |
    |--- TCP poll_rx --------->|                      |
    |                          |                      |
    |                   TLS reassembly buffer          |
    |                   (accumulate TCP segments)      |
    |                          |                      |
    |                   TLS record parse + decrypt     |
    |                   (AEAD open, strip content type)|
    |                          |                      |
    |                   WS frame decode               |
    |                   (FrameProcessor::process)      |
    |                     |         |                  |
    |                  control    data                 |
    |                  frames     frames               |
    |                     |         |                  |
    |                  ping->pong   |                  |
    |                  close->resp  |                  |
    |                               |                  |
    |                   [on_message callback]          |
    |                   or SPSC try_produce            |
    |                               |                  |
    |                          +-- [RxMsg] ----------->|
    |                                           recv() |
```

### TX Path (Direct Transport)

```
App Thread (single thread, no queues)
    |
    |  send(data, len)
    |  -> UTF-8 check (text)
    |  -> WS frame encode
    |  -> [TLS encrypt]
    |  -> TCP send
    |
    |  poll() or feed_rx() + process_pending()
    |  -> TCP poll_rx
    |  -> [TLS reassembly + decrypt]
    |  -> WS frame decode (FrameProcessor)
    |  -> on_message callback
```

## Key Components

### Transport::create
```cpp
template <TcpTransport TcpImpl, MessageFramer Framer = WsFramer, ...>
[[nodiscard]] static std::expected<std::unique_ptr<Transport>, ConnectionErrorInfo>
Transport::create(TcpFactory tcp_factory, const TransportConfig& config);
```
Factory that performs blocking TCP + TLS 1.3 + WebSocket handshake, then starts TX/RX threads (unless `deferred_start`).

### Transport::send
```cpp
[[nodiscard]] SendError send(const void* data, size_t len,
                             uint8_t opcode = ws::opcode::kBinary) noexcept;
```
Non-blocking enqueue to TX SPSC queue. Returns `kOk` on enqueue success (not wire delivery).

### Transport::recv
```cpp
template <typename F> requires std::invocable<F, const uint8_t*, size_t, uint8_t>
[[nodiscard]] bool recv(F&& callback);
```
Non-blocking dequeue from RX SPSC queue. Callback receives `(data, len, opcode)`.

### TransportConfig::from_url
```cpp
[[nodiscard]] static std::expected<TransportConfig, std::string>
from_url(std::string_view url);
```
Parses `ws://` or `wss://` URLs into a config, setting host, port, path, and TLS mode.

### DirectTransport::poll
```cpp
[[nodiscard]] std::expected<uint16_t, std::string> poll() noexcept;
```
Self-driven: TCP poll + TLS decrypt + WS decode in a single call. Returns segment count.

### DirectTransport::feed_rx / process_pending
```cpp
void feed_rx(const uint8_t* data, uint16_t len) noexcept;
void process_pending() noexcept;
```
Split API for Reactor integration: feed_rx accumulates raw bytes, process_pending decrypts and decodes.

### TlsRecordCrypto::encrypt / decrypt
```cpp
uint16_t encrypt(const uint8_t* plaintext, uint16_t plaintext_len, uint8_t* out) noexcept;
bool decrypt(const uint8_t* record, uint16_t record_len, uint8_t* out, uint16_t& out_len) noexcept;
```
Hot-path AEAD: AES-GCM seal/open with TLS 1.3 nonce construction and sequence tracking.

### ReconnectPolicy::attempt
```cpp
[[nodiscard]] bool attempt(
    std::function<std::expected<void, ConnectionErrorInfo>()> connect_fn) noexcept;
```
One reconnection attempt: sleep (exponential backoff + jitter), call connect_fn, update state.

### make_twophase_filter
```cpp
inline FrameFilterFn make_twophase_filter(
    std::function<uint32_t(const uint8_t* data, size_t len)> extractor);
```
Creates a batch frame filter that delivers only the latest frame per symbol hash.

### ws::encode_frame / ws::decode_frame
```cpp
[[nodiscard]] inline size_t encode_frame(uint8_t* out, uint8_t opcode,
    const uint8_t* payload, uint64_t payload_len, bool fin = true) noexcept;
[[nodiscard]] inline std::expected<DecodedFrame, DecodeError>
decode_frame(const uint8_t* data, size_t len);
```
RFC 6455 wire format: client masking with batch-pregenerated keys, zero-copy decode.

## Entry Points and APIs

| Entry Point | Transport Variant | Threading | Use Case |
|-------------|-------------------|-----------|----------|
| `Transport<T>::create()` | Threaded | TX thread + RX thread | General-purpose non-blocking send/recv |
| `DirectTxTransport<T>::create()` | Direct TX | App thread TX, RX thread | Lowest TX latency with async RX |
| `DirectTransport<T>::create()` | Threadless | App thread only | Single-threaded event loops (Reactor, DPDK) |
| `DefaultTransport<T>` | Threaded preset | TX + RX threads | 512B payload, 1024-deep queues |
| `SmallTransport<T>` | Threaded preset | TX + RX threads | 64B payload, 256-deep queues |
| `LargeTransport<T>` | Threaded preset | TX + RX threads | 4096B payload, 512-deep queues |
| `EvictTransport<T>` | Threaded preset | TX + RX threads | Overwrites oldest unread on RX overflow |
| `RawTransport<T>` | Threaded preset | TX + RX threads | No WS framing, raw byte delivery |
| `DirectTxDefaultTransport<T>` | Direct TX preset | App TX, RX thread | Direct TX, WS framing, 512B/1024 |
| `DirectDefaultTransport<T>` | Direct preset | App thread only | Threadless WS, 512B payload |

## Dependencies

### Internal Module Graph

```
presets.hpp
    |
    +-- transport.hpp
    |       +-- tx_worker.hpp
    |       +-- rx_worker.hpp
    |       |       +-- frame_processor.hpp
    |       +-- reconnect_policy.hpp
    |       +-- transport_core.hpp
    |               +-- tls_session.hpp
    |               +-- tls_record.hpp
    |               |       +-- tls_encryptor.hpp
    |               |       +-- tls_decryptor.hpp
    |               |       +-- tls_constants.hpp
    |               +-- websocket.hpp
    |               +-- http.hpp
    |               +-- ws_framer.hpp
    |               +-- transport_types.hpp
    |               +-- detail/message_types.hpp
    |
    +-- direct_transport.hpp
    |       +-- frame_processor.hpp
    |       +-- reconnect_policy.hpp
    |       +-- transport_core.hpp
    |       +-- tls_encryptor.hpp
    |       +-- tls_decryptor.hpp
    |
    +-- direct_tx_transport.hpp
            +-- rx_worker.hpp
            +-- reconnect_policy.hpp
            +-- transport_core.hpp
            +-- tls_encryptor.hpp
```

### External Packages

| Package | Usage | Headers |
|---------|-------|---------|
| eph-core | TcpTransport/MessageFramer concepts, error types, JSON escape | `tcp_concept.hpp`, `framer_concept.hpp`, `transport_errors.hpp`, `detail/json_escape.hpp` |
| eph-containers | Lock-free SPSC queues | `bounded_queue.hpp`, `evicting_queue.hpp` |
| eph-utils | HDR histogram, TSC timing, CPU affinity, cache-line alignment | `hdr_histogram.hpp`, `time.hpp`, `cpu.hpp`, `alignment.hpp` |
| spdlog | Structured leveled logging | `spdlog/spdlog.h`, `spdlog/sinks/stdout_color_sinks.h` |
| OpenSSL / aws-lc | TLS 1.3 handshake (libssl), AES-GCM AEAD (libcrypto), HKDF, SHA-1, RAND | `openssl/ssl.h`, `openssl/aead.h`, `openssl/evp.h`, `openssl/hkdf.h`, `openssl/rand.h`, `openssl/bio.h`, `openssl/mem.h` |

## Testing

Test files covering eph-transport functionality are located in `tests/net/`:

| Test File | Coverage |
|-----------|----------|
| `test_transport.cpp` | Transport lifecycle, send/recv, reconnection, stats |
| `test_transport_types.cpp` | TransportConfig validation, URL parsing, stats serialization |
| `test_socket_transport.cpp` | Integration with kernel socket TCP backend |
| `test_websocket.cpp` | WS frame encode/decode, masking, close/ping/pong, UTF-8 |
| `test_http.cpp` | HTTP upgrade request/response, Sec-WebSocket-Accept validation |
| `test_tls_record.cpp` | TLS record encrypt/decrypt, nonce construction, sequence limits |
| `test_framer.cpp` | WsFramer and RawFramer concept satisfaction and round-trip |
| `test_tcp_concept.cpp` | TcpTransport concept constraint checks |

Additional tests in `tests/core/`:

| Test File | Coverage |
|-----------|----------|
| `test_transport_errors.cpp` | SendError, ConnectionError, ConnectionErrorInfo formatting |
