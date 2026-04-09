# Project: eph-transport

> Header-only C++23 WebSocket / TLS 1.3 transport abstraction layered above
> any TCP backend satisfying the `TcpTransport` concept (kernel sockets via
> `eph-net`, DPDK poll-mode via `eph-dpdk`, or a mock).

**Language**: C++23 | **Build**: xmake (header-only) | **Deps**: eph-core,
eph-containers, eph-utils, spdlog, aws-lc

---

## Table of Contents
1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Module Map](#module-map)
4. [Data Flow](#data-flow)
5. [Key Components](#key-components)
6. [Entry Points & APIs](#entry-points--apis)
7. [Dependencies](#dependencies)
8. [Testing](#testing)

---

## Overview

`eph-transport` is the network transport layer of the `eph` stack. It sits
between a TCP backend and application-level parsers, providing an encrypted
(TLS 1.3) WebSocket (or raw-framed) bytestream with pluggable threading
models. The library is entirely templated over its TCP backend through the
`TcpTransport` concept defined in `eph-core`; the same code compiles against
`eph-net` kernel sockets and against `eph-dpdk` DPDK poll-mode TCP.

Three class templates expose three threading models that share a common
core (`TransportCore`, `FrameProcessor`, `ReconnectPolicy`, TLS stack,
WebSocket state machine):

- `Transport<>`   - TX thread + RX thread, both backed by SPSC queues.
- `DirectTxTransport<>` - application thread sends directly (no TX queue),
  RX still runs on a background thread with a queue.
- `DirectTransport<>`   - no threads, no queues; the application drives both
  TX and RX inline. Built for single-threaded event loops (Reactor,
  io_uring, DPDK poll-mode).

All three variants use a custom AES-GCM AEAD fast path on top of aws-lc
exported session keys, bypassing `SSL_read` / `SSL_write` on the hot path
after the handshake completes. TLS handshake and WebSocket Upgrade still
use aws-lc and the minimal `http::` helpers respectively. Reconnection is
handled by a standalone `ReconnectPolicy` using exponential backoff with
+/-25% jitter.

The library is header-only: `xmake build eph-transport` is a no-op that
only validates dependencies. Consumers instantiate the templates with
their chosen TCP backend.

---

## Architecture

Layered and composition-based. Each top-level transport class owns:

1. A `TransportCore<TcpImpl>` value member - shared connection state
   (TCP, TLS crypto, config, lifecycle atomics, connection metadata).
2. A `ReconnectPolicy` value member - backoff state machine.
3. Variant-specific workers / processors:
   - `Transport`         - `TxWorker` + `RxWorker`.
   - `DirectTxTransport` - `RxWorker` only; TX is inline via
     `TransportCore::send_*_direct()`.
   - `DirectTransport`   - a `FrameProcessor` driven by the caller; TX
     inline, RX via `poll()` or `feed_rx()` / `process_pending()`.

The framing layer is pluggable via the `MessageFramer` concept, with two
built-in implementations: `WsFramer` (RFC 6455) and `RawFramer`
(pass-through, for SOH-delimited / length-prefixed protocols).

### Component Diagram

```
  +--------------------------------------------------------------+
  |                  Application (on_message, send)             |
  +--------------------------------------------------------------+
            |                      |                   ^
            v                      v                   |
  +-----------------+  +-------------------+  +------------------+
  |   Transport<>   |  | DirectTxTransport |  |  DirectTransport |
  |  (TX+RX thread) |  |   (RX thread)     |  |  (no threads)    |
  +-----------------+  +-------------------+  +------------------+
     |   |   |              |   |                   |
     v   v   v              v   v                   v
  +-----+-----+----+    +-----+-----+         +---------------+
  |TxWk |RxWk | RP |    |Core |RxWk |         |Core | FP | RP |
  +-----+-----+----+    +-----+-----+         +---------------+
     shared by all variants:
  +-----------------------------------------------------------+
  | TransportCore<TcpImpl>: TcpImpl, TlsSession, TlsRecord-   |
  |   Crypto, TransportConfig, lifecycle atomics, metadata    |
  +-----------------------------------------------------------+
     |                 |                 |                 |
     v                 v                 v                 v
  +--------+   +--------------+   +----------+   +------------+
  |TcpImpl |   | TlsSession / |   | WsFramer |   | http::     |
  | (net/  |   | TlsRecord-   |   | RawFramer|   | upgrade    |
  |  dpdk) |   | Crypto       |   | (concept)|   | helpers    |
  +--------+   +--------------+   +----------+   +------------+
                    ^
                    |
                 aws-lc (libssl handshake, libcrypto AES-GCM)
```

---

## Module Map

| Module / File | Responsibility | Key Types | Depends On |
|---|---|---|---|
| `transport.hpp` | Threaded transport class template with TX+RX workers and queues | `Transport<>` | core, tx_worker, rx_worker, reconnect_policy, ws_framer |
| `direct_tx_transport.hpp` | RX-thread-only variant; app sends synchronously | `DirectTxTransport<>` | core, rx_worker, reconnect_policy, ws_framer |
| `direct_transport.hpp` | Threadless variant driven by caller (`poll`, `feed_rx`/`process_pending`) | `DirectTransport<>` | core, frame_processor, reconnect_policy, http, tls_session, websocket |
| `presets.hpp` | Canonical payload/depth/framer alias templates | `DefaultTransport`, `SmallTransport`, `LargeTransport`, `EvictTransport`, `RawTransport`, `DirectTx*`, `Direct*` | transport, direct_tx_transport, direct_transport |
| `transport_types.hpp` | Public enums, `TransportConfig`, stats, callbacks, URL helpers | `TransportConfig`, `TransportState`, `TransportEvent`, `TransportStats`, `ConnectionInfo`, `RttStats`, `TransportStateCallback` | eph-core transport_errors, json_escape |
| `reconnect_policy.hpp` | Exponential backoff with +/-25% jitter, attempt accounting | `ReconnectPolicy` | transport_types, message_types |
| `ws_framer.hpp` | `MessageFramer` adapter over `detail/websocket.hpp` | `WsFramer` | framer_concept, detail/websocket |
| `raw_framer.hpp` | Pass-through `MessageFramer` (no framing overhead) | `RawFramer` | framer_concept |
| `detail/transport_core.hpp` | Shared connection state: TCP, TLS crypto, config, lifecycle atomics, handshake metadata, send_*_direct helpers | `TransportCore<TcpImpl>` | tcp_concept, tls_session, tls_record, tls_encryptor, websocket, http, ws_framer |
| `detail/tx_worker.hpp` | TX thread, TX SPSC queue, ping scheduling, TLS seq monitor | `TxWorker<>`, `TxWorkerStats` | bounded_queue, tls_record, websocket, message_types |
| `detail/rx_worker.hpp` | RX thread, RX SPSC or evicting queue, decrypt/decode loop via FrameProcessor | `RxWorker<>`, `RxWorkerStats`, `ReceivedMessage` | bounded_queue, evicting_queue, frame_processor, tls_record, message_types |
| `detail/frame_processor.hpp` | Reusable RX decode pipeline: TLS decrypt, WS decode, fragment reassembly, control-frame handling, batch filter | `FrameProcessor<...>` | websocket, frame_filter, tls_record |
| `detail/frame_filter.hpp` | Batch `FrameView` view + two-phase hash-based dedup filter for multi-symbol streams | `FrameView`, `FrameFilterFn`, `make_twophase_filter` | - |
| `detail/websocket.hpp` | RFC 6455 encode/decode, masking, opcode/close-code helpers, UTF-8 validation, frame templates | `ws::opcode::*`, `ws::close_code::*`, `encode_frame`, `decode_frame`, `MaskKeyCache`, `FrameTemplate` | - |
| `detail/http.hpp` | Minimal HTTP/1.1 client for the WebSocket Upgrade only | `generate_ws_key`, `build_upgrade_request`, `parse_upgrade_response`, `validate_ws_accept` | aws-lc (SHA-1, base64) |
| `detail/tls_session.hpp` | TLS 1.3 handshake via aws-lc custom BIO bound to a `TcpTransport`, exports session keys | `TlsSession<TcpImpl>` | aws-lc libssl, tcp_concept |
| `detail/tls_record.hpp` | Composed `TlsEncryptor` + `TlsDecryptor` (`TlsRecordCrypto`) + record-layer helpers | `TlsRecordCrypto`, `tls_record::*` | tls_encryptor, tls_decryptor, tls_constants |
| `detail/tls_encryptor.hpp` | Write-side AES-128/256-GCM AEAD, owns TX key/IV/sequence | `TlsEncryptor` | aws-lc libcrypto, tls_constants |
| `detail/tls_decryptor.hpp` | Read-side AES-128/256-GCM AEAD, owns RX key/IV/sequence | `TlsDecryptor` | aws-lc libcrypto, tls_constants |
| `detail/tls_constants.hpp` | TLS record layout constants, nonce construction, max-record size, sequence ceiling | `tls_const::*` | - |
| `detail/message_types.hpp` | Cache-aligned TX/RX message structs with optional TSC timestamp | `TxMessage<N>`, `RxMessage<N>` | eph-utils TSC |
| `detail/logger.hpp` | Single shared spdlog logger accessor (`transport_logger()`) | - | spdlog |

---

## Data Flow

The TX path is strictly one-way: application payload -> (optional SPSC
enqueue) -> frame encode (WS or raw) -> TLS AEAD encrypt (if TLS on) ->
`TcpImpl::send`. The RX path reverses that: `TcpImpl` receive -> (optional
TLS reassembly buffer) -> TLS AEAD decrypt -> WS decode / fragment
reassembly / control-frame handling -> (optional batch filter) ->
`on_message` callback or RX queue.

In the threaded variant both pipelines run on dedicated busy-poll threads
with SPSC queues decoupling them from the application. `DirectTxTransport`
keeps the RX thread but runs TX inline on the caller. `DirectTransport`
removes both threads; the caller drives the RX pipeline via `poll()` (or
`feed_rx()` + `process_pending()` for Reactor integration).

TLS handshake and WebSocket Upgrade run once on the control thread inside
`create()`; the `TlsSession` object is released after it exports the AEAD
keys into `TlsRecordCrypto`, so the hot path no longer involves libssl.
Reconnection is orchestrated by the RX worker (or the direct-transport's
`poll()`) and driven by a `ReconnectPolicy` instance.

### Flow Diagram

```
  [app send(data,len,op)]
          |
          v (Transport: enqueue; Direct/DirectTx: inline)
   +---------------+     +----------------+     +-------------+
   | TX SPSC queue | --> | WsFramer /     | --> | TlsRecord-  |
   | (optional)    |     | RawFramer      |     | Crypto enc  |
   +---------------+     | encode()       |     | (AES-GCM)   |
                         +----------------+     +-------------+
                                                       |
                                                       v
                                                +-------------+
                                                | TcpImpl     |
                                                | ::send()    |
                                                +-------------+

                                                +-------------+
                                                | TcpImpl     |
                                                | ::recv()    |
                                                +-------------+
                                                       |
                                                       v
   +--------------+     +----------------+     +-------------+
   | on_message   | <-- | FrameProcessor | <-- | TlsRecord-  |
   | / RX queue   |     |  WS decode +   |     | Crypto dec  |
   +--------------+     |  reassembly +  |     | (AES-GCM)   |
                        |  ctrl frames + |     +-------------+
                        |  batch filter  |
                        +----------------+
```

---

## Key Components

### `Transport<TcpImpl, Framer, MaxPayload, QueueDepth, RxQueueTmpl, LastOnlyDeliver>`

**File**: `include/eph/transport/transport.hpp`
**Purpose**: Default threaded transport. Owns TX worker, RX worker, core
and reconnect policy. Non-movable, heap-allocated via `std::unique_ptr`
returned from the static `create()` factory.
**Interface**:
```
static expected<unique_ptr<Transport>, ConnectionErrorInfo>
    create(TcpFactory, const TransportConfig&);
SendError send(const void*, size_t, uint8_t opcode = kBinary);
SendError send_text(...); send_binary(...); send_close(...); send_ping(...);
SendError send_n(span<const uint8_t>*, size_t, uint8_t opcode);
template<F> bool recv(F&&); bool try_recv(...); bool wait_recv(F, timeout);
void start_threads();   // only if deferred_start
bool close_gracefully(code, reason, timeout);
void stop();
TransportState state() const; bool is_running() const; bool is_connected();
bool reconnect_now();
TransportStats stats() const; const TransportConfig& config() const;
```
**Notes**:
- `send()` returning `SendError::kOk` means the message was enqueued in
  the TX SPSC queue, **not** that it was sent on the wire. At-least-once
  delivery requires an application-level acknowledgement.
- `MaxPayload` is statically clamped to `tls_const::kMaxRecordPayload`
  (16384 B); `QueueDepth` must be a power of two.
- `TcpFactory` is called both for the initial connect and for every
  reconnection attempt, so it must be idempotent and produce a fresh
  `TcpImpl`.
- All RX-thread callbacks (`on_message`, `on_close`, `on_ping`, `on_pong`,
  `on_rx_drop`, `on_reconnect_attempt`, `on_reconnected`) run on the RX
  thread and must be non-blocking. Buffers passed to `on_message` are
  valid only during the callback invocation.

### `DirectTxTransport<TcpImpl, Framer, MaxPayload, QueueDepth, RxQueueTmpl, LastOnlyDeliver>`

**File**: `include/eph/transport/direct_tx_transport.hpp`
**Purpose**: Removes the TX thread and TX queue from the `Transport`
variant. The application thread encodes, encrypts and TCP-sends inline via
`TransportCore::send_*_direct()`. The RX side still uses a dedicated
thread + queue.
**Interface**: Same as `Transport<>` minus `tx_queue_*` observability and
the `send_for*` timeout overloads. `send()` is synchronous.
**Notes**:
- Thread-safety invariant: the application thread exclusively owns
  `crypto->enc` (TX direction); the RX thread exclusively owns
  `crypto->dec`. Concurrent `send()` and `stop()` from different threads
  is undefined.
- `tx_queue_hwm` in `TransportStats` is always 0.

### `DirectTransport<TcpImpl, Framer, MaxPayload>`

**File**: `include/eph/transport/direct_transport.hpp`
**Purpose**: Threadless transport for single-threaded event loops. Hosts
a `FrameProcessor` with `DirectDeliver` (calls `on_message` inline) and
`DirectSendFn` (routes control-frame replies through `send_direct_`).
**Interface**:
```
static expected<unique_ptr<DirectTransport>, ConnectionErrorInfo>
    create(TcpFactory, const TransportConfig&);
SendError send(const void*, size_t, uint8_t opcode = kBinary);
SendError send_text(...); send_binary(...); send_close(...); send_ping(...);
SendError send_n(span<const uint8_t>*, size_t, uint8_t);
void feed_rx(const uint8_t* data, uint16_t len);     // accumulate only
void process_pending();                              // drain buffers
void poll();                                         // tcp poll + process
void stop(); bool is_running() const;
TransportState state() const; bool is_connected() const;
TransportStats stats() const;
```
**Notes**:
- No RX queue exists - data is delivered only through the `on_message`
  callback on `TransportConfig`.
- `feed_rx()` and `process_pending()` must be called from the same
  thread; they are **not** internally synchronised.
- Reassembly buffers are owned by the transport instance and sized for
  one TLS record plus one WS fragment context.

### `TransportCore<TcpImpl>`

**File**: `include/eph/transport/detail/transport_core.hpp`
**Purpose**: Shared connection state for every transport variant. Owns
`TcpImpl`, `TlsSession` (during handshake), `TlsRecordCrypto` (hot path),
`TransportConfig`, lifecycle atomics (`running`, `reconnecting`,
`closing`, `force_reconnect`, `close_requested`), negotiated TLS / cipher /
subprotocol strings, handshake timings, pong tracking, and TSC conversion
state. Provides `do_connect()`, `do_ws_upgrade<Framer>()`,
`send_close_direct()`, `send_ping_direct()`, and `notify_state()`.
**Notes**: Exposed as a struct with public fields because workers need
direct access. Encapsulation is enforced one level up by the owning
`Transport*` class.

### `RxWorker<TcpImpl, Framer, MaxPayload, QueueDepth, RxQueueTmpl, LastOnlyDeliver>`

**File**: `include/eph/transport/detail/rx_worker.hpp`
**Purpose**: Owns the RX thread, RX queue (bounded or evicting), RX stats
(including latency histograms when timestamps are enabled), and a
`FrameProcessor`. Drives TLS reassembly, decryption and frame decoding.
Also owns the RX-side reconnection loop - `stop()` synchronises with any
in-progress reconnect to avoid use-after-free on TLS/TCP state.
**Notes**: The `LastOnlyDeliver` template parameter skips all but the last
WebSocket data frame per `process_ws_data()` batch; useful for
single-symbol streams. `RxQueueTmpl` selects `BoundedQueue` (drop-newest)
or `EvictingQueue` (drop-oldest) semantics.

### `TxWorker<TcpImpl, Framer, MaxPayload, QueueDepth>`

**File**: `include/eph/transport/detail/tx_worker.hpp`
**Purpose**: TX thread, TX SPSC queue, ping scheduling, TLS sequence
monitoring, TX stats and histograms. Consumes `TxMessage<MaxPayload>`
objects, encodes them with `Framer::encode`, encrypts with
`TlsRecordCrypto::encrypt`, and sends through `TcpImpl::send`. Emits
periodic pings based on `TransportConfig::ping_interval`.

### `FrameProcessor<TcpImpl, Framer, Deliver, SendFn, MaxPayload, LastOnlyDeliver>`

**File**: `include/eph/transport/detail/frame_processor.hpp`
**Purpose**: Reusable RX decode pipeline shared by `RxWorker` and
`DirectTransport`. Handles WebSocket fragment reassembly, control-frame
responses (Pong on Ping, Close echo), batch indexing for
`on_frame_filter`, and deliver / skip dispatch. Parameterised on a
`Deliver` policy (writes to a queue or calls `on_message` directly) and a
`SendFn` policy (how to emit control-frame replies).

### `ReconnectPolicy`

**File**: `include/eph/transport/reconnect_policy.hpp`
**Purpose**: Standalone, testable exponential-backoff state machine.
`attempt(connect_fn)` sleeps `current_backoff_ * jitter`, invokes
`connect_fn`, on success returns true and increments `total_reconnects_`;
on failure doubles the backoff (capped at `max_reconnect_backoff` or 16x
`reconnect_interval` when unset) and notifies `on_reconnect_attempt`. The
caller decides when to give up by checking `exhausted()`.
**Notes**: +/-25% jitter is applied via a thread-local `std::mt19937`
seeded with multiple entropy sources so reconnecting fleets do not
synchronise.

### `TransportConfig`

**File**: `include/eph/transport/transport_types.hpp`
**Purpose**: User-facing configuration struct covering connection target,
TLS settings, timeouts, performance tuning, reconnection, WebSocket
keepalive, CPU affinity, UTF-8 validation mode and all optional
callbacks. Provides `validate()`, `warnings()`, `dump()`, `to_json()`,
`to_url()`, `from_url()` and `operator==`.
**Notes**: All callback fields are invoked from the RX thread (except
TX-side state change notifications) and must be non-blocking. Use
`deferred_start = true` to delay thread startup until
`start_threads()` is called - needed when the application wants to
install shared RX ring state between handshake and first RX poll.

### `WsFramer` / `RawFramer`

**Files**: `include/eph/transport/ws_framer.hpp`,
`include/eph/transport/raw_framer.hpp`
**Purpose**: Implement the `MessageFramer` concept. `WsFramer` wraps
`ws::encode_frame` / `ws::decode_frame` and exposes
`max_overhead() == ws::kMaxFrameHeaderLen` (14 bytes). `RawFramer` is a
pass-through (`max_overhead() == 0`) used when the upstream protocol
manages its own framing.

### Zero-copy and lifetime notes (honest)

- `WsFramer::encode()` masks the payload while writing into the caller's
  output buffer, so the encoded frame is not the same memory as the input
  payload - there is one pass of masked copy on the TX path. `RawFramer`
  is a straight `memcpy`. "Zero-copy" in this codebase refers to avoiding
  heap allocation, not to avoiding all payload copies.
- RX delivery via `on_message` or `recv()` exposes a pointer into the
  transport's internal reassembly / queue buffers. The pointer is valid
  only for the duration of the callback (or until the next `recv()` /
  `process_pending()` call for the direct variant). Callers that need the
  data beyond that window must copy it.
- TLS encrypt / decrypt operate in-place into caller-provided buffers
  (`TlsRecordCrypto::encrypt` / `::decrypt`); the composed
  `TlsEncryptor` / `TlsDecryptor` own no scratch heap allocations on the
  hot path.
- The TX / RX SPSC queues store `TxMessage<MaxPayload>` / `RxMessage<..>`
  value objects; payloads live inline in cache-aligned slots - no heap
  allocation per send / receive.
- Ownership: all transport classes are non-copyable, non-movable, and
  RAII-owned via `std::unique_ptr`. `TcpImpl` is owned by
  `TransportCore::tcp` and replaced on reconnect via `TcpFactory`.
  `TlsSession` is destroyed once its key material has been exported into
  `TlsRecordCrypto`; behaviour of `TlsSession` reuse across reconnect is
  governed by `do_connect()` rebuilding the session from scratch.

---

## Entry Points & APIs

| Entrypoint | Type | Description |
|---|---|---|
| `Transport<>::create()` | static factory | Builds the threaded variant, runs TCP connect, TLS handshake and WS Upgrade, then starts workers (unless `deferred_start`). |
| `DirectTxTransport<>::create()` | static factory | Same handshake flow, starts only the RX worker. |
| `DirectTransport<>::create()` | static factory | Same handshake flow, starts no threads. |
| `Transport<>::send*` / `recv*` | member | Non-blocking enqueue / dequeue; `send_for*` and `wait_recv*` add timeouts. |
| `DirectTxTransport<>::send*` | member | Synchronous encode/encrypt/TCP-send on caller thread. |
| `DirectTransport<>::poll()` | member | All-in-one driver: TCP poll + TLS decrypt + WS decode + deliver. |
| `DirectTransport<>::feed_rx()` + `process_pending()` | member | Reactor split: accumulate raw bytes, then drain. |
| `Transport<>::close_gracefully()` | member | Sends WS Close, waits up to timeout for peer echo, then `stop()`. |
| `Transport<>::reconnect_now()` | member | Signals the RX thread to drop the current connection and reconnect. |
| `TransportConfig::from_url()` | static | Parses `ws://` / `wss://` URLs into a populated `TransportConfig`. |
| `TransportConfig::validate()` / `warnings()` | member | Eager config sanity checks before `create()`. |
| `TransportStats::operator-` | free | Snapshot subtraction for windowed metrics. |
| `make_twophase_filter()` | free | Builds a batch frame filter closure for multi-symbol dedup. |
| `ws::encode_frame` / `decode_frame` | free | Low-level WebSocket primitives (re-exported via `detail/websocket.hpp`). |
| `http::build_upgrade_request` / `parse_upgrade_response` | free | Minimal HTTP/1.1 WebSocket handshake helpers. |

---

## Dependencies

### Internal (module graph)

```
                 +-----------------+
                 |    presets.hpp  |
                 +--------+--------+
                          |
        +-----------------+-----------------+
        |                 |                 |
        v                 v                 v
  +-----------+   +-----------------+  +-----------------+
  | transport |   | direct_tx_      |  | direct_         |
  | .hpp      |   | transport.hpp   |  | transport.hpp   |
  +-----+-----+   +--------+--------+  +--------+--------+
        |                  |                    |
        v                  v                    v
  +----------+      +----------+         +----------------+
  |tx_worker |      |rx_worker |         |frame_processor |
  +----+-----+      +----+-----+         +--------+-------+
       |                 |                        |
       +--------+--------+------------------------+
                |        |
                v        v
           +---------+  +-------------+
           |transport|  | ws_framer / |
           |_core    |  | raw_framer  |
           +----+----+  +------+------+
                |              |
                v              v
     +----------+----------+  +--------------+
     | tls_session         |  | frame_filter |
     | tls_record (enc/dec)|  +--------------+
     | websocket / http    |
     +----------+----------+
                |
                v
     +----------------------+
     | transport_types.hpp  |  <-- TransportConfig, stats, enums
     +----------+-----------+
                |
                v
     +----------------------+
     | message_types /      |
     | logger / tls_const   |
     +----------------------+
```

### External

| Package | Role | Scope (xmake) |
|---|---|---|
| `eph-core` | `TcpTransport` / `MessageFramer` concepts, transport error enums, JSON-escape and control-char helpers | public dep |
| `eph-containers` | `BoundedQueue`, `EvictingQueue` (lock-free SPSC queues) | public dep |
| `eph-utils` | `TSC`, `HdrHistogram`, CPU affinity, `cpu_relax`, cache-line alignment | public dep |
| `spdlog` | Leveled logging via `SPDLOG_LOGGER_*` (compile-time filter via `SPDLOG_ACTIVE_LEVEL`) | public package |
| `aws-lc` | TLS 1.3 handshake (libssl) + AES-GCM AEAD (libcrypto) | public package |
| `eph-net` | Only pulled into test binaries for end-to-end TCP coverage (not linked by the library itself) | test-only dep |

Compile-time knob: `-DEPH_ENABLE_TIMESTAMPS=1` turns on per-message TSC
timestamps and the HdrHistogram latency breakdowns. When off, the `tsc`
fields in `TxMessage` / `RxMessage` are unused and the histogram hot-path
calls compile to no-ops.

---

## Testing

| Test Suite | Location | Coverage Focus |
|---|---|---|
| `test_framers` | `tests/test_framers.cpp` | `WsFramer` / `RawFramer` contract - encode roundtrip, decode partial/complete, error mapping |
| `test_http` | `tests/test_http.cpp` | HTTP/1.1 Upgrade request build, response parse, `Sec-WebSocket-Accept` validation |
| `test_reconnect_policy` | `tests/test_reconnect_policy.cpp` | Exponential backoff growth, jitter bounds, `attempt()` / `exhausted()` / `reset()` state machine, reconnect callback |
| `test_tls_config` | `tests/test_tls_config.cpp` | TLS field validation in `TransportConfig` and associated warnings |
| `test_tls_record` | `tests/test_tls_record.cpp` | AEAD encrypt / decrypt roundtrip, record header build/parse, sequence-number ceiling enforcement |
| `test_transport_config` | `tests/test_transport_config.cpp` | `TransportConfig::validate`, `warnings`, `dump`, `to_json`, `to_url`, `from_url`, `operator==` |
| `test_transport_types` | `tests/test_transport_types.cpp` | `TransportStats` delta arithmetic, rate helpers, `ConnectionInfo` formatting, enum name helpers |
| `test_websocket` | `tests/test_websocket.cpp` | Frame encode / decode, fragmentation, masking, close handshake, UTF-8 validation, opcode / close-code helpers, frame templates |

All test targets are generated from `tests/**.cpp` by `xmake.lua` using the
`eph-test` rule and link `eph-net` so they can exercise the transport over
real kernel sockets. No test binary is built for `detail/rx_worker.hpp`,
`detail/tx_worker.hpp`, `detail/frame_processor.hpp` or the three
top-level transport classes directly; behaviour is covered indirectly via
the framer / TLS / config tests plus end-to-end coverage that lives in
`eph-net` and benchmarks. Dedicated coverage for `direct_transport.hpp`
and `direct_tx_transport.hpp` as entry points is TBD - the core decode
pipeline they host is exercised by `test_websocket` and `test_framers`,
but the `feed_rx()` / `process_pending()` boundary has no dedicated unit
test in this module.

Benchmarks live in `benchmarks/bench_transport_types.cpp` and cover the
hot-path primitives: `encode_frame` (64 B and 512 B), `apply_mask`,
AES-GCM encrypt / decrypt, HKDF, `TransportStats::dump` / `to_json`,
`TransportConfig::from_url`, and TLS record parsing. Built via the
`eph-bench` xmake rule.

Fuzzing is provided by `fuzzers/fuzz_ws_decode.cpp`, a libFuzzer harness
targeting `ws::decode_frame` against arbitrary byte inputs.

Key test scenarios:
- Frame encode / decode symmetry for binary, text and control frames.
- Fragmented WebSocket messages reassembled across multiple TCP reads.
- RFC 6455 close-code validation including reserved / application ranges.
- UTF-8 validation of text frames and close reasons (RFC 6455 sec 5.6, sec 7.1.6).
- TLS record AEAD roundtrip with sequence numbers approaching the
  `kMaxSequenceNumber` ceiling.
- Reconnect policy reaches `exhausted()` exactly at
  `max_reconnect_attempts` and jitter stays inside the +/-25% band.
- `TransportConfig` round-trips through `to_url()` / `from_url()` and
  flags invalid hosts, ports and mismatched mTLS pairs in `validate()`.
