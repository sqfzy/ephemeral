# eph-dpdk

Header-only C++23 library for ultra-low-latency WebSocket (WSS) communication over DPDK, bypassing the kernel network stack entirely.

```
Application                     eph-dpdk                           NIC
    │                              │                                │
    │  send(data, len)             │                                │
    ├─────────────────►  SPSC Queue ──► TX Thread                   │
    │                       WS Frame Encode (masked_copy)           │
    │                       TLS 1.3 AEAD Seal (AES-NI)             │
    │                       TCP/IP Header Build (template)          │
    │                       ─────────────────────────► tx_burst() ──┤
    │                                                               │
    │                              RX Thread ◄──────── rx_burst() ──┤
    │                       TCP process_rx (seq/ack)                │
    │                       TLS Record Reassembly + AEAD Open       │
    │                       WS Frame Decode                         │
    │  recv(callback)  ◄── SPSC Queue                               │
    │                              │                                │
```

## Performance

Measured on Intel Xeon (2.69 GHz, AES-NI enabled), release build:

| Stage | 64B | 256B | 1024B |
|-------|-----|------|-------|
| WS Encode | 6ns | 13ns | 45ns |
| TLS Encrypt (AES-GCM) | 148ns | 172ns | 302ns |
| TLS Decrypt (AES-GCM) | 138ns | 173ns | 309ns |
| **E2E TX** (app call -> wire-ready) | **164ns** | **245ns** | **441ns** |
| **E2E RX** (wire -> app callback) | **139ns** | **165ns** | **376ns** |

126 unit tests, all passing. Covers boundary conditions, error paths, and crypto edge cases.

## Architecture

Three-layer design, all header-only under `include/eph/dpdk/`:

```
Layer 3: Transport API
  transport.hpp ─── Public send()/recv()/stop(), thread management,
                    SPSC queues, auto-reconnect, WS ping keepalive

Layer 2: Protocol Stack
  tcp.hpp ────────── Minimal user-space TCP (seq/ack, no retransmission)
  tls_session.hpp ── TLS 1.3 handshake via aws-lc custom BIO
  tls_record.hpp ─── AEAD record encrypt/decrypt (EVP_AEAD_CTX_seal/open)
  websocket.hpp ──── RFC 6455 framing, batch-cached CSPRNG masking
  http.hpp ────────── HTTP/1.1 Upgrade (WebSocket handshake only)

Layer 1: DPDK Platform
  eal.hpp ─────────── EAL lifecycle (once per process)
  platform.hpp ────── Port/queue/mempool initialization
  net_header.hpp ──── Ethernet/IPv4/TCP headers, checksum, packet template
```

## PMD (Poll Mode Driver) — How eph-dpdk Talks to NICs

DPDK doesn't use the kernel network stack. Instead, it talks to NICs directly through **PMD (Poll Mode Drivers)**. Think of a PMD as a userspace NIC driver — each NIC vendor provides one (or DPDK provides generic ones).

eph-dpdk itself is **PMD-agnostic**: it only uses DPDK's abstract port/queue API (`rte_eth_rx_burst`, `rte_eth_tx_burst`). Which PMD is used is decided at startup through EAL (Environment Abstraction Layer) arguments — the same way you'd pass command-line flags to configure a program.

### Choosing a PMD

| PMD | When to Use | Pros | Cons |
|-----|-------------|------|------|
| **af_packet** | Development, any Linux NIC | Works everywhere, no special hardware | Higher latency (~10us overhead vs native PMD) |
| **mlx5** | Production (Mellanox/NVIDIA ConnectX) | Zero-copy, HW checksum offload, lowest latency | Requires specific NIC + firmware |
| **net_pcap** | Offline testing, debugging | Read/write pcap files, no NIC needed | Not for live traffic |

### Configuration Examples

PMD selection happens when you call `eal_init()`. The EAL arguments tell DPDK which driver and device to use:

```cpp
// ─── Option 1: AF_PACKET ───────────────────────────────────────────
// Uses the kernel's AF_PACKET socket to send/receive raw Ethernet frames.
// Works on ANY Linux NIC — no driver binding or hugepages needed.
// Good for: development, testing, or when you can't bind a NIC to DPDK.
const char* args[] = {
    "myapp",
    "--vdev", "net_af_packet0,iface=eth0",  // wrap kernel NIC "eth0"
    "--no-huge",                             // don't require hugepages
    nullptr
};
eal_init(4, const_cast<char**>(args));
// After this, port_id=0 refers to "eth0" via AF_PACKET.

// ─── Option 2: Native PMD (e.g. mlx5, i40e, ixgbe) ────────────────
// Binds a physical NIC directly to DPDK — kernel loses access to it.
// Requires: NIC bound to vfio-pci or uio_pci_generic driver first.
//   $ sudo dpdk-devbind.py -b vfio-pci 0000:03:00.0
const char* args[] = {
    "myapp",
    "-a", "0000:03:00.0",  // PCI address of the NIC
    nullptr
};
eal_init(3, const_cast<char**>(args));
// After this, port_id=0 refers to the physical NIC at that PCI address.

// ─── Option 3: PCAP (file-based, no NIC) ───────────────────────────
// Reads packets from a pcap file and writes sent packets to another.
// Great for: unit testing, replaying captured traffic, CI pipelines.
const char* args[] = {
    "myapp",
    "--vdev", "net_pcap0,rx_pcap=input.pcap,tx_pcap=output.pcap",
    "--no-huge",
    nullptr
};
eal_init(4, const_cast<char**>(args));
// After this, port_id=0 reads from input.pcap, writes to output.pcap.
```

After `eal_init()`, the rest of eph-dpdk code is identical regardless of which PMD is active — `Platform::create({.port_id = 0})` and `Transport::create(pool, config)` work the same way.

## Quick Start

```cpp
#include <eph/dpdk/eal.hpp>
#include <eph/dpdk/platform.hpp>
#include <eph/dpdk/transport.hpp>

int main(int argc, char** argv) {
    // 1. Initialize DPDK EAL
    auto eal = eph::dpdk::eal_init(argc, argv);
    if (!eal) { std::cerr << eal.error() << '\n'; return 1; }

    // 2. Initialize NIC port
    auto platform = eph::dpdk::Platform::create({
        .port_id = 0,
        .nb_rx_queues = 1, .nb_tx_queues = 1,
        .mbuf_pool_size = 8191,
    });
    if (!platform) { std::cerr << platform.error() << '\n'; return 1; }

    // 3. Create WSS transport (TCP + TLS 1.3 + WebSocket handshake)
    eph::dpdk::TransportConfig config{
        .remote_host = "stream.example.com",
        .remote_port = 443,
        .ws_path     = "/ws/v1",

        .tuple = {
            .src_ip   = eph::dpdk::net::parse_ipv4("10.0.0.100"),
            .dst_ip   = eph::dpdk::net::parse_ipv4("203.0.113.1"),
            .src_port = 12345,
            .dst_port = 443,
        },
        .src_mac = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}},
        .dst_mac = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}}, // gateway MAC

        .tx_cpu = 2,   // pin TX thread to core 2
        .rx_cpu = 3,   // pin RX thread to core 3
    };

    auto transport = eph::dpdk::DefaultTransport::create(
        platform->mempool(), config);
    if (!transport) { std::cerr << transport.error() << '\n'; return 1; }
    auto& t = *transport;

    // 4. Send (non-blocking)
    const char* msg = R"({"method":"SUBSCRIBE","params":["ticker"]})";
    t->send_text(msg, strlen(msg));

    // 5. Receive (non-blocking, poll in loop)
    while (t->is_running()) {
        t->recv([](const uint8_t* data, uint16_t len) {
            // Process market data — data valid only during callback
        });
    }

    t->stop();
    eph::dpdk::eal_cleanup();
}
```

## Transport API

```cpp
// Template parameters: MaxPayload (bytes), QueueDepth (must be power of 2)
using DefaultTransport = Transport<512, 1024>;
using SmallTransport   = Transport<64, 256>;
using LargeTransport   = Transport<4096, 512>;

// Factory (blocking — performs TCP + TLS + WS handshake)
static std::expected<std::unique_ptr<Transport>, std::string>
    Transport::create(rte_mempool* pool, const TransportConfig& config);

// Send (non-blocking, returns errno: 0, -EAGAIN, -EMSGSIZE, -ENOTCONN)
int send(const void* data, size_t len,
         uint8_t opcode = ws::opcode::kBinary);
int send_text(const void* data, size_t len);  // convenience for JSON APIs

// Receive (non-blocking, callback pattern)
bool recv(auto&& callback);  // callback(const uint8_t* data, uint16_t len)

// Lifecycle
void stop();
bool is_running();
TransportStats stats();
```

## TCP Design

Minimal user-space TCP state machine — designed for low-latency exchange feeds, not general-purpose networking.

| Implements | Does NOT Implement |
|---|---|
| seq/ack tracking | Retransmission |
| ACK generation | Nagle / delayed ACK |
| Window management | Congestion control |
| FIN/RST handling | SACK |
| WebSocket ping/pong | TCP timestamps |

**Loss strategy**: detect out-of-order/loss -> immediate reconnect (~2ms). This is acceptable for datacenter environments with near-zero packet loss.

## TLS Design

- **Handshake**: aws-lc (BoringSSL fork) via custom BIO backed by the user-space TCP session
- **Data plane**: Direct `EVP_AEAD_CTX_seal/open` (single-call AEAD), bypassing `SSL_write/SSL_read`
- **Key extraction**: `SSL_get_{read,write}_traffic_secret` + HKDF-Expand-Label (NOT exporter keys)
- **Sequence sync**: `SSL_get_{read,write}_sequence` after WS upgrade phase
- **Cipher support**: AES-128-GCM and AES-256-GCM (dynamic, based on negotiated cipher)

## AF_PACKET Deployment Notes

When using `net_af_packet` PMD on a real NIC:

```bash
# Disable GRO — merged packets create TCP sequence gaps that trigger reconnect
sudo ethtool -K eth0 gro off

# Block kernel RST/ACK — kernel doesn't know about eph-dpdk's TCP connections
# and will send RST for "unknown" SYN-ACK packets. Use nftables:
sudo nft add table inet eph_filter
sudo nft add chain inet eph_filter output '{ type filter hook output priority 0; }'
sudo nft add rule inet eph_filter output \
    tcp sport 12345 tcp flags rst counter drop
```

## Dependencies

| Package | Purpose |
|---|---|
| [DPDK](https://www.dpdk.org/) | NIC PMD, mbuf, EAL, ethdev |
| [aws-lc](https://github.com/aws/aws-lc) | TLS 1.3 (SSL), AES-GCM (EVP_AEAD), SHA-1 (EVP), CSPRNG (RAND) |
| [spdlog](https://github.com/gabime/spdlog) | Structured logging with compile-time level filtering |

Build dependencies (test/bench only): [Google Test](https://github.com/google/googletest), [Google Benchmark](https://github.com/google/benchmark)

## Build

```bash
# Build library (header-only, just validates compilation)
xmake build eph-dpdk

# Build and run tests (no DPDK EAL required)
xmake build -g tests
xmake run test_net_header
xmake run test_tls_record
xmake run test_websocket
xmake run test_http

# Build and run benchmarks
xmake build bench_ws_pipeline
xmake run bench_ws_pipeline
```

## License

Part of the [ephemeral](https://github.com/user/ephemeral) project.
