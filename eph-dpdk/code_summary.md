# Project: eph-dpdk

> Header-only C++23 library providing a low-latency WebSocket-over-TLS-over-TCP transport built entirely on DPDK user-space networking — bypassing the kernel for sub-millisecond market data connectivity.

**Language**: C++23 | **Build**: Header-only (part of `ephemeral` monorepo) | **License**: —

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

eph-dpdk is a high-performance networking library that implements a complete WSS (WebSocket Secure) client stack on top of DPDK. Instead of relying on the kernel's TCP/IP stack, it constructs Ethernet/IPv4/TCP packets directly in user space, performs TLS 1.3 encryption via aws-lc's AEAD API, and frames application data as WebSocket binary messages — all with zero system calls on the data path.

The library is designed for latency-sensitive applications (e.g. HFT market data feeds) where every microsecond matters. It uses a split architecture: a **control plane** (single-threaded handshake: TCP 3-way → TLS 1.3 → WebSocket Upgrade) and a **data plane** (dedicated TX/RX threads communicating through lock-free SPSC queues). After handshake, the TLS session keys are extracted and handed to a lightweight `TlsRecordCrypto` engine that uses single-call `EVP_AEAD_CTX_seal/open` — avoiding the overhead of the full OpenSSL `SSL_write/SSL_read` path.

Key design choices include: no TCP retransmission (packet loss triggers immediate reconnect at ~2ms), constexpr configuration validation, cache-line-aligned TLS key material to prevent false sharing between TX/RX cores, and batch-pregenerated WebSocket mask keys to amortize CSPRNG cost.

All modules are header-only under `include/eph/dpdk/` and live in the `eph::dpdk` namespace.

---

## Architecture

The architecture follows a **layered pipeline** model. Each layer handles one protocol concern, and the layers compose vertically: Platform → TCP → TLS → WebSocket → Transport.

The **Transport** class orchestrates the full stack and exposes a simple `send()`/`recv()` API to application code. Internally it separates connection setup (blocking, single-threaded) from data transfer (non-blocking, multi-threaded with SPSC queues).

### Component Diagram

```
┌──────────────────────────────────────────────┐
│              Application Thread               │
│         send(data, len) / recv(cb)           │
└──────────┬──────────────────────┬────────────┘
           │ SPSC TxQueue        │ SPSC RxQueue
┌──────────▼──────────┐ ┌───────▼─────────────┐
│     TX Thread        │ │     RX Thread        │
│  WS encode → TLS    │ │  TLS decrypt → WS    │
│  encrypt → TCP send  │ │  decode → push queue │
└──────────┬──────────┘ └───────┬─────────────┘
           │                    │
┌──────────▼────────────────────▼──────────────┐
│              TcpSession                       │
│   seq/ack tracking, ACK gen, FIN/RST, ISN    │
│   (no retransmission — loss = reconnect)     │
└──────────┬────────────────────┬──────────────┘
           │ tx_burst           │ rx_burst
┌──────────▼────────────────────▼──────────────┐
│              Platform (DPDK)                  │
│   EAL init → mempool → port config →         │
│   queue setup → NIC start → link poll        │
└──────────────────────────────────────────────┘
```

### Handshake Sequence (Control Plane)

```
  TCP 3-Way Handshake        TLS 1.3 Handshake
  ──────────────────         ──────────────────
  SYN ──────────►            ClientHello ────►
  ◄────── SYN+ACK            ◄──── ServerHello
  ACK ──────────►                  ...
                              ◄──── Finished
                              Finished ──────►

  WS Upgrade                  Key Export
  ──────────                  ──────────
  GET /path HTTP/1.1 ────►    extract_hot_state()
  ◄──── 101 Switching         → TlsRecordCrypto
  validate Sec-WS-Accept      (AEAD seal/open)
```

---

## Module Map

| Module / File | Responsibility | Key Types | Depends On |
|---|---|---|---|
| `include/eph/dpdk/eal.hpp` | EAL lifecycle (init/cleanup) | `eal_init()`, `eal_cleanup()` | DPDK `rte_eal` |
| `include/eph/dpdk/platform.hpp` | NIC port setup, mempool, queue config | `Platform`, `PlatformConfig`, `Stats` | DPDK `rte_ethdev`, `rte_mbuf` |
| `include/eph/dpdk/net_header.hpp` | Ethernet/IPv4/TCP headers, checksums, packet build/parse | `PacketTemplate`, `ParsedPacket`, `ConnectionTuple` | DPDK `rte_ether`, `rte_ip`, `rte_tcp` |
| `include/eph/dpdk/tcp.hpp` | User-space TCP state machine (handshake, data, close) | `TcpSession`, `TcpState`, `TcpConfig` | `net_header.hpp`, OpenSSL `RAND_bytes` |
| `include/eph/dpdk/tls_session.hpp` | TLS 1.3 handshake via custom BIO, key extraction | `TlsSession`, `TlsHotState`, `TlsKeyMaterial`, `BioContext` | `tcp.hpp`, aws-lc/OpenSSL |
| `include/eph/dpdk/tls_record.hpp` | AEAD record encrypt/decrypt (hot path) | `TlsRecordCrypto` | `tls_session.hpp`, aws-lc `EVP_AEAD` |
| `include/eph/dpdk/http.hpp` | Minimal HTTP/1.1 for WebSocket Upgrade | `UpgradeResponse`, `build_upgrade_request()`, `validate_ws_accept()` | OpenSSL `EVP_sha1` |
| `include/eph/dpdk/websocket.hpp` | WebSocket frame encode/decode (RFC 6455) | `DecodedFrame`, `FrameTemplate`, `MaskKeyCache` | OpenSSL `RAND_bytes` |
| `include/eph/dpdk/transport.hpp` | Full-stack WSS transport (public API) | `Transport<>`, `TransportConfig`, `TransportStats` | All above + `eph::containers::BoundedQueue`, `eph::base::cache` |

---

## Data Flow

### Send Path (Application → Network)

Application data enters through `Transport::send()`, which copies the payload into a fixed-size `TxMessage` and pushes it into a lock-free SPSC queue. The TX thread drains up to 32 messages per batch, encodes each as a masked WebSocket binary frame (`FrameTemplate::encode`), encrypts the frame into a TLS 1.3 application data record (`TlsRecordCrypto::encrypt` via `EVP_AEAD_CTX_seal`), then sends the encrypted bytes through `TcpSession::send` which builds Ethernet/IPv4/TCP packets on mbufs and calls `rte_eth_tx_burst`.

### Receive Path (Network → Application)

The RX thread calls `rte_eth_rx_burst` to poll for incoming packets, passes them through `TcpSession::process_rx` which validates TCP sequence numbers, extracts payloads, and sends ACKs. TCP payloads accumulate in a reassembly buffer. Complete TLS records are decrypted via `TlsRecordCrypto::decrypt` (`EVP_AEAD_CTX_open`), then decoded as WebSocket frames. Data frames are pushed into the RX SPSC queue; control frames (ping/close) are handled inline. The application calls `Transport::recv()` with a callback to consume messages.

### Flow Diagram

```
 Application send(data, len)
          │
          ▼
   ┌─ SPSC TxQueue ─┐
   │  TxMessage[N]   │
   └───────┬─────────┘
           ▼
   WS Frame Encode (masked_copy + XOR)
           │
           ▼
   TLS Encrypt (EVP_AEAD_CTX_seal)
           │
           ▼
   TCP Segment (PacketTemplate::build_packet)
           │
           ▼
   rte_eth_tx_burst ──────► NIC


   NIC ──────► rte_eth_rx_burst
                     │
                     ▼
           TcpSession::process_rx
           (seq check, ACK gen)
                     │
                     ▼
           Reassembly Buffer
                     │
                     ▼
           TLS Decrypt (EVP_AEAD_CTX_open)
                     │
                     ▼
           WS Frame Decode
                     │
              ┌──────┴──────┐
              │             │
         Data frame    Control frame
              │         (ping → pong,
              ▼          close → stop)
   ┌─ SPSC RxQueue ─┐
   │  RxMessage[N]   │
   └───────┬─────────┘
           ▼
   Application recv(callback)
```

---

## Key Components

### `Transport<MaxPayload, QueueDepth>`

**File**: `include/eph/dpdk/transport.hpp`
**Purpose**: Top-level public API. Orchestrates the full WSS connection lifecycle and provides non-blocking send/recv to application code.
**Interface**:
```cpp
static std::expected<std::unique_ptr<Transport>, std::string>
create(rte_mempool* pool, const TransportConfig& config);

int send(const void* data, size_t len) noexcept;  // -EAGAIN, -EMSGSIZE, -ENOTCONN

template <typename F>
bool recv(F&& callback) noexcept;  // callback(const uint8_t*, uint16_t)

void stop() noexcept;
TransportStats stats() const noexcept;
```
**Notes**: Non-movable (owns threads). Uses `unique_ptr` factory pattern. Template params control SPSC queue sizing — `QueueDepth` must be power of 2. Auto-reconnect on disconnect with configurable interval and max attempts. Discards stale TX queue data during reconnect.

### `TcpSession`

**File**: `include/eph/dpdk/tcp.hpp`
**Purpose**: Minimal user-space TCP state machine. Handles 3-way handshake, seq/ack tracking, window management, FIN/RST.
**Interface**:
```cpp
std::expected<void, std::string> connect(std::chrono::milliseconds timeout);
std::expected<size_t, std::string> send(const void* data, size_t len);
template <typename F>
std::expected<uint16_t, std::string> process_rx(rte_mbuf** pkts, uint16_t nb, F&& cb);
std::expected<void, std::string> close();
```
**Notes**: Intentionally omits retransmission, Nagle, delayed ACK, congestion control. Out-of-order packets trigger an error (caller reconnects). ISN generated via CSPRNG (`RAND_bytes`). Sequence wraparound handled by signed comparison.

### `TlsRecordCrypto`

**File**: `include/eph/dpdk/tls_record.hpp`
**Purpose**: Hot-path TLS 1.3 record encryption/decryption using aws-lc AEAD API.
**Interface**:
```cpp
static std::expected<TlsRecordCrypto, std::string>
create(const TlsHotState& state, size_t key_len);

uint16_t encrypt(uint8_t* plaintext, uint16_t len, uint8_t* out) noexcept;
bool decrypt(const uint8_t* record, uint16_t len, uint8_t* out, uint16_t& out_len) noexcept;
```
**Notes**: Thread-safe for split TX/RX usage (separate AEAD contexts). Encrypt requires 1 byte of writable space past the plaintext for the TLS 1.3 inner content type — avoids a full memcpy. Nonce construction uses `uint64_t` XOR optimization instead of byte loop.

### `TlsSession`

**File**: `include/eph/dpdk/tls_session.hpp`
**Purpose**: TLS 1.3 handshake over DPDK TCP via custom BIO, plus session key extraction for hot-path AEAD takeover.
**Interface**:
```cpp
static std::expected<TlsSession, std::string>
create(TcpSession& tcp, rte_mempool* pool, const TlsConfig& config);

std::expected<void, std::string> handshake();
std::expected<TlsHotState, std::string> extract_hot_state() const;
```
**Notes**: Custom `BIO_METHOD` bridges OpenSSL I/O to `TcpSession` send/recv. After handshake + key extraction, the `SSL*` object is only used for shutdown — data plane bypasses it entirely. `TlsKeyMaterial` is cache-line-aligned (64 bytes) to prevent false sharing between TX/RX threads.

### `PacketTemplate` / `ParsedPacket`

**File**: `include/eph/dpdk/net_header.hpp`
**Purpose**: Zero-copy Ethernet/IPv4/TCP packet construction and parsing directly on DPDK mbufs.
**Interface**:
```cpp
// Build
rte_mbuf* build_packet(rte_mempool* pool, uint32_t seq, uint32_t ack,
                        uint8_t flags, uint16_t window,
                        const void* payload, uint16_t payload_len) noexcept;
// Parse
ParsedPacket parse_packet(const rte_mbuf* mbuf) noexcept;
```
**Notes**: Supports both software checksums and NIC TX offload (`hw_cksum` flag). `fill_packet` variant reuses pre-allocated mbufs for the hot path. Parser uses `ip->total_length` instead of `pkt_len` to avoid NIC padding corruption.

### `Platform`

**File**: `include/eph/dpdk/platform.hpp`
**Purpose**: DPDK NIC initialization: port enumerate → mempool create → port configure → queue setup → start → link poll.
**Interface**:
```cpp
static std::expected<Platform, std::string> create(const PlatformConfig& config);
rte_mempool* mempool() const noexcept;
Stats collect_stats() const;
```
**Notes**: `PlatformConfig` is constexpr-validatable — `static_assert(config_ok(cfg))` works at compile time. Descriptor counts are clamped to NIC hardware limits. Offload flags are intersected with device capabilities to prevent portability bugs.

---

## Entry Points & APIs

| Entrypoint | Type | Description |
|---|---|---|
| `eph::dpdk::eal_init()` | Init | Initialize DPDK EAL (once per process) |
| `Platform::create()` | Init | Configure NIC port and mempool |
| `Transport<>::create()` | Factory | Full WSS connection (TCP+TLS+WS handshake) |
| `Transport<>::send()` | Data | Non-blocking send (app thread → SPSC → TX) |
| `Transport<>::recv()` | Data | Non-blocking recv (RX → SPSC → app thread) |
| `Transport<>::stop()` | Lifecycle | Graceful shutdown (WS Close + TCP FIN) |

Type aliases for common configurations:
- `DefaultTransport` = `Transport<512, 1024>`
- `SmallTransport` = `Transport<64, 256>` (control messages)
- `LargeTransport` = `Transport<4096, 512>` (bulk data)

---

## Dependencies

### Internal (module graph)

```
transport ──► tcp ──────────► net_header
    │         ▲
    ├──► tls_record ──► tls_session ──► tcp
    │
    ├──► websocket
    │
    ├──► http
    │
    ├──► eph::containers::BoundedQueue (SPSC queue)
    │
    └──► eph::base::cache (CACHE_LINE_SIZE)
```

### External

| Library | Purpose |
|---|---|
| DPDK (`rte_eal`, `rte_ethdev`, `rte_mbuf`, `rte_mempool`) | User-space NIC access, packet I/O, memory management |
| aws-lc / BoringSSL (`openssl/ssl.h`, `openssl/aead.h`, `openssl/hkdf.h`) | TLS 1.3 handshake, AEAD encryption, HKDF key derivation, CSPRNG |
| spdlog | Structured logging with compile-time level filtering |

---

## Testing

| Test Suite | Location | Coverage Focus |
|---|---|---|
| — | — | No test files found in eph-dpdk |

**Note**: The project currently has no dedicated test directory. Testing likely requires DPDK-enabled hardware (or vdev) and a remote WSS endpoint, making it inherently integration-test-oriented. The constexpr validation utilities (`validate_config`, `clamp_desc`, `is_power_of_two_minus_one`) are good candidates for compile-time static assertions and unit tests.
