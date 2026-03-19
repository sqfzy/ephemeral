# Project: eph-dpdk

> Ultra-low-latency DPDK WebSocket/TLS transport library for HFT market data reception.

**Language**: C++23 (header-only) | **Build**: xmake | **Crypto**: aws-lc (BoringSSL fork)

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
9. [Performance](#performance)

---

## Overview

**eph-dpdk** is a header-only C++23 library that implements a complete WebSocket-over-TLS communication stack on top of DPDK, bypassing the kernel network stack entirely. It targets HFT (High-Frequency Trading) systems that connect to exchange WebSocket feeds where every nanosecond of latency matters.

The library provides a user-space TCP state machine, TLS 1.3 encryption via aws-lc's AEAD API, RFC 6455 WebSocket framing with optimized client masking, and a thread-safe Transport API that ties everything together with SPSC lock-free queues.

Key performance characteristics (256B payload, measured on 2.69 GHz Xeon):
- **E2E TX**: ~500ns (application call to wire-ready packet)
- **E2E RX**: ~170ns (wire to application callback)
- **TLS encrypt**: ~142ns (AES-256-GCM via AES-NI)
- **WS encode**: ~57ns (batch-cached masking keys + fused memcpy/XOR)

The library builds on three companion modules: **eph-base** (cache-line constants, concepts), **eph-utils** (TSC timer, HdrHistogram recorder), and **eph-containers** (SPSC lock-free queues).

---

## Architecture

The library follows a **three-layer architecture** with strict separation between platform initialization, protocol stack, and public API. Each layer is header-only and independently testable.

```
┌──────────────────────────────────────────────────────┐
│                  Layer 3: Transport                  │
│  transport.hpp — public API, thread management,      │
│  SPSC queues, auto-reconnect, WS ping keepalive      │
├──────────────────────────────────────────────────────┤
│               Layer 2: Protocol Stack                │
│  ┌──────────┐ ┌────────────┐ ┌───────────────┐      │
│  │ tcp.hpp  │ │tls_session │ │ tls_record.hpp│      │
│  │ TCP FSM  │ │  .hpp      │ │ AEAD enc/dec  │      │
│  │ seq/ack  │ │ handshake  │ │ EVP_AEAD_seal │      │
│  └────┬─────┘ │ key export │ └───────────────┘      │
│       │       └────────────┘                         │
│  ┌────┴──────────┐ ┌──────────┐ ┌──────────┐        │
│  │net_header.hpp │ │http.hpp  │ │websocket │        │
│  │Eth/IP/TCP hdr │ │WS Upgrade│ │  .hpp    │        │
│  │checksum, parse│ │request   │ │RFC 6455  │        │
│  └───────────────┘ └──────────┘ └──────────┘        │
├──────────────────────────────────────────────────────┤
│              Layer 1: DPDK Platform                  │
│  eal.hpp — EAL lifecycle (once per process)          │
│  platform.hpp — port/queue/mempool management        │
└──────────────────────────────────────────────────────┘
```

### Thread Model

```
  Application Thread          TX Worker Thread         RX Worker Thread
  ───────────────────         ─────────────────        ─────────────────
  send(data, len)             drain SPSC TX queue      rx_burst()
       │                           │                        │
       ▼                           ▼                        ▼
  SPSC TX Queue ──────►  WS encode + mask         TCP process_rx()
                          TLS AEAD seal                    │
                          TCP send()                       ▼
                               │                   TLS AEAD open
                               ▼                   WS decode
                          tx_burst()                      │
                                                          ▼
                                                  SPSC RX Queue ──► recv(cb)
```

---

## Module Map

| Module | Lines | Responsibility | Key Types | Depends On |
|--------|-------|----------------|-----------|------------|
| `eal.hpp` | 45 | DPDK EAL lifecycle | `eal_init()`, `eal_cleanup()` | DPDK |
| `platform.hpp` | 497 | NIC port/queue/mempool | `Platform`, `PlatformConfig`, `Stats` | DPDK, spdlog |
| `net_header.hpp` | 459 | Ethernet/IP/TCP headers | `PacketTemplate`, `ParsedPacket`, `ConnectionTuple` | DPDK |
| `tcp.hpp` | 575 | User-space TCP FSM | `TcpSession`, `TcpConfig`, `TcpState` | net_header, DPDK, aws-lc |
| `tls_session.hpp` | 586 | TLS 1.3 handshake + key export | `TlsSession`, `TlsHotState`, `TlsKeyMaterial` | tcp, aws-lc |
| `tls_record.hpp` | 315 | AEAD record encrypt/decrypt | `TlsRecordCrypto` | tls_session, aws-lc |
| `http.hpp` | 234 | HTTP Upgrade (WS only) | `UpgradeResponse`, `build_upgrade_request()` | aws-lc |
| `websocket.hpp` | 434 | RFC 6455 framing | `MaskKeyCache`, `FrameTemplate`, `DecodedFrame` | aws-lc |
| `transport.hpp` | 911 | Public API + threads | `Transport<N,Q>`, `TransportConfig`, `TransportStats` | all above + bounded_queue |

---

## Data Flow

### TX Path (send → wire)

```
  Application payload (uint8_t*, len)
           │
           ▼
  ┌─── SPSC TX Queue (lock-free) ───┐
  │  TxMessage{data[], len, opcode} │
  └──────────────┬──────────────────┘
                 │  TX worker thread drains batch
                 ▼
  ┌─── WS Frame Encode ────────────┐
  │  header(6-14B) + masked_copy() │  ◄── MaskKeyCache (batch CSPRNG)
  └──────────────┬─────────────────┘
                 ▼
  ┌─── TLS Record Seal ────────────┐
  │  EVP_AEAD_CTX_seal (AES-NI)   │  nonce = IV ⊕ seq (uint64 XOR)
  │  [hdr(5)][ciphertext][tag(16)] │
  └──────────────┬─────────────────┘
                 ▼
  ┌─── TCP Send ───────────────────┐
  │  PacketTemplate.build_packet() │  Eth+IP+TCP headers + checksum
  │  rte_eth_tx_burst()            │  (or HW offload if supported)
  └────────────────────────────────┘
```

### RX Path (wire → recv)

```
  rte_eth_rx_burst()
           │
           ▼
  ┌─── TCP process_rx() ──────────┐
  │  seq/ack validate, ACK gen    │  Out-of-order → reconnect
  │  append payload to reassembly │
  └──────────────┬────────────────┘
                 ▼
  ┌─── TLS Record Open ───────────┐
  │  EVP_AEAD_CTX_open (AES-NI)   │  Tampered → reconnect
  └──────────────┬─────────────────┘
                 ▼
  ┌─── WS Frame Decode ───────────┐
  │  parse opcode, payload_len    │
  │  ping → auto pong response    │
  │  close → stop transport       │
  │  data → push to RX queue      │
  └──────────────┬─────────────────┘
                 ▼
  ┌─── SPSC RX Queue ─────────────┐
  │  RxMessage{data[], len}       │
  └────────────────────────────────┘
           │
           ▼
  Application recv(callback)
```

---

## Key Components

### `Transport<MaxPayload, QueueDepth>`

**File**: `transport.hpp`
**Purpose**: High-level API tying all layers together. Manages worker threads, SPSC queues, auto-reconnect, and WS ping keepalive.

```cpp
// Create (blocking — performs TCP+TLS+WS handshake)
auto result = Transport<512, 1024>::create(pool, config);
auto& transport = *result;  // unique_ptr<Transport>

// Send (non-blocking, returns errno)
int err = transport->send(data, len);  // 0, -EAGAIN, -EMSGSIZE, -ENOTCONN

// Receive (non-blocking, callback pattern)
transport->recv([](const uint8_t* data, uint16_t len) {
    // data valid only during this callback
});

// Stop (graceful: Close frame → join threads → TCP FIN)
transport->stop();
```

**Design decisions**:
- Non-movable (owns threads) — returned via `unique_ptr`
- Per-thread stats (no atomic contention on hot path)
- Fixed-interval reconnect with configurable max attempts
- TX queue drain on reconnect (market data = stale messages discarded)

---

### `TlsRecordCrypto`

**File**: `tls_record.hpp`
**Purpose**: AES-256-GCM record encryption/decryption via aws-lc's single-call AEAD API.

```cpp
auto crypto = TlsRecordCrypto::create(hot_state);
// Encrypt: plaintext → [record_header][ciphertext][tag]
uint16_t n = crypto->encrypt(plaintext, len, out);
// Decrypt: [record_header][ciphertext][tag] → plaintext
bool ok = crypto->decrypt(record, record_len, out, out_len);
```

**Design decisions**:
- `EVP_AEAD_CTX_seal/open` (single call) vs Init/Update/Final (eliminates ~150ns overhead)
- Separate enc/dec contexts → TX and RX threads can operate concurrently
- Nonce construction: `uint64_t` XOR optimization (4-byte memcpy + 8-byte XOR vs 12-byte loop)
- Zero-copy encrypt: temporarily appends content type byte past plaintext, then restores

---

### `TcpSession`

**File**: `tcp.hpp`
**Purpose**: Minimal user-space TCP state machine — seq/ack tracking, window management, FIN/RST. No retransmission.

```cpp
TcpSession tcp(config, pool);
tcp.connect(timeout);                    // Three-way handshake via DPDK
tcp.send(data, len);                     // Build and tx_burst
tcp.process_rx(pkts, nb_pkts, callback); // Parse, ACK, deliver payload
tcp.close();                             // FIN handshake
```

**Design decisions**:
- ISN generated via `RAND_bytes` (CSPRNG, RFC 6528 compliant)
- Out-of-order detection → error (triggers reconnect at Transport level)
- HW checksum offload support (`PacketTemplate::hw_cksum` flag)

---

### `MaskKeyCache`

**File**: `websocket.hpp`
**Purpose**: Eliminates per-frame `RAND_bytes()` (~1500ns) by batch-generating 1024 mask keys upfront.

```cpp
// Thread-local, refills automatically
MaskKeyCache& cache = mask_key_cache();
cache.next_key(mask);  // ~2ns (index increment + memcpy)
```

---

### `PacketTemplate`

**File**: `net_header.hpp`
**Purpose**: Pre-filled Ethernet/IP/TCP header template. Hot path only updates dynamic fields (seq, ack, length, checksum).

```cpp
PacketTemplate tmpl{.src_mac=..., .dst_mac=..., .tuple=..., .hw_cksum=true};
rte_mbuf* pkt = tmpl.build_packet(pool, seq, ack, flags, window, payload, len);
```

---

## Entry Points & APIs

| Entrypoint | Type | Description |
|---|---|---|
| `Transport::create()` | Factory | Full connection establishment (blocking) |
| `Transport::send()` | Hot path | Non-blocking enqueue, returns errno |
| `Transport::recv()` | Hot path | Non-blocking dequeue with callback |
| `Transport::stop()` | Lifecycle | Graceful shutdown |
| `Transport::stats()` | Query | Merged TX/RX statistics |
| `eal_init()` | Global init | DPDK EAL (once per process) |
| `Platform::create()` | Factory | NIC port initialization |

---

## Dependencies

### Internal Module Graph

```
transport ──► tcp ──► net_header
    │         │
    │         └──► (DPDK rte_*)
    │
    ├──► tls_session ──► tcp
    │         │
    │         └──► (aws-lc SSL/BIO)
    │
    ├──► tls_record ──► tls_session
    │         │
    │         └──► (aws-lc EVP_AEAD)
    │
    ├──► websocket ──► (aws-lc RAND)
    │
    ├──► http ──► (aws-lc EVP SHA-1)
    │
    ├──► eph-base/cache.hpp (CACHE_LINE_SIZE = 64)
    │
    └──► eph-containers/bounded_queue.hpp (SPSC)
```

### External Packages

| Package | Purpose |
|---|---|
| `dpdk` | NIC PMD, mbuf, EAL, ethdev |
| `aws-lc` | TLS 1.3 (SSL), AES-256-GCM (EVP_AEAD), SHA-1 (EVP), CSPRNG (RAND) |
| `spdlog` | Structured logging with compile-time level filtering |
| `gtest` | Unit testing framework |
| `benchmark` | Google Benchmark for latency measurement |

---

## Testing

| Test Suite | File | Tests | Coverage Focus |
|---|---|---|---|
| WebSocket | `test_websocket.cpp` | 29 | encode/decode roundtrip (15 payload sizes), masking symmetry, integer overflow, control frames, FrameTemplate, MaskKeyCache |
| TLS Record | `test_tls_record.cpp` | 23 | AEAD seal/open roundtrip (11 sizes), sequence tracking, tampered record rejection, nonce monotonicity, content type validation |
| HTTP | `test_http.cpp` | 15 | Upgrade request format, 101 response parsing, case-insensitive headers, RFC 6455 Sec-WebSocket-Accept (reference vector), base64 |
| Net Header | `test_net_header.cpp` | 14 | hton/ntoh constexpr roundtrip, RFC 1071 checksum, TCP checksum self-verification, IPv4 parse/format, ConnectionTuple equality |
| **Total** | | **81** | All pass |

Tests do NOT require DPDK EAL — they exercise pure logic functions only.

---

## Performance

Benchmark: `bench_ws_pipeline.cpp` (Google Benchmark, AES-NI enabled)

| Stage | 64B | 256B | 1024B | Scaling |
|-------|-----|------|-------|---------|
| Checksum | 47ns | 210ns | 882ns | O(n) |
| WS Masking | 36ns | 75ns | 262ns | O(n) |
| WS Encode | 29ns | 57ns | 151ns | O(n) |
| WS Decode | 11ns | 12ns | 12ns | O(1) |
| TLS Encrypt | 115ns | 142ns | 296ns | ~O(n) |
| TLS Decrypt | 149ns | 177ns | 342ns | ~O(n) |
| TCP Hdr Build | 119ns | 280ns | 1033ns | O(n) checksum |
| TCP Hdr Parse | 10ns | 10ns | 10ns | O(1) |
| **E2E TX** | **289ns** | **504ns** | **1452ns** | |
| **E2E RX** | **154ns** | **187ns** | **370ns** | |
