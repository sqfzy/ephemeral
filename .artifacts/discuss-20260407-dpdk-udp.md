# Discussion Record — UDP Support Layer Design

## Context
- Time: 2026-04-07 ~09:00 (second, deeper discussion)
- Duration: ~15 minutes
- User request: Design the UDP support layer for eph-dpdk. The library is a TCP-centric DPDK networking library for ultra-low-latency trading systems. Currently it has: UDP header definitions (net_header.hpp), DNS over UDP (dns.hpp), and multicast UDP reception (multicast.hpp). Missing: UdpSession, UdpTransport, UDP unicast send/recv, UDP connector, UDP reactor support, UDP flow steering, UDP checksum wrapper, IP fragmentation. The design should maintain the library's header-only, zero-copy, compile-time-driven philosophy. Key considerations: (1) UDP is connectionless vs TCP's stateful sessions - what abstraction fits? (2) Integration with existing transport layer (eph-transport) (3) Flow steering and reactor support for UDP (4) Whether to support reliability/retransmission or keep it pure UDP (5) Checksum offload to NIC vs software (6) API ergonomics - should it mirror TCP connector pattern or have its own idiom?
- Complexity: High
- Rounds: 7 (converged at round 6-7, no new substantive arguments)
- Participants: R5 First-Principles Thinker, R3 Performance Fanatic, R14 Architect, R2 Minimalist, R6 Maintainability Advocate

## Summary
The discussion centered on what UDP abstraction level eph-dpdk needs. R5 framed the scope by questioning real use cases (only TX unicast is currently needed). R3 pushed for a precomputed packet template for deterministic latency. R14 ensured architectural consistency with TCP's two-layer pattern (PacketTemplate + Session). R2 constrained scope to TX-only, deferring Reactor/RX/concept integration. R6 ensured test coverage and future extensibility. Final consensus: lightweight two-layer TX abstraction (UdpPacketTemplate + UdpSender), extend Flow Steering for UDP, defer RX/Reactor/concept, refactor DNS in separate PR.

---

## Round 1 — Independent Positions

**R5 (First-Principles):** Questioned the premise — what UDP use cases actually exist beyond already-solved DNS and multicast? Proposed stateless tools (UdpTxHandle + UdpRxFilter) instead of session-like classes. UDP is connectionless; don't mimic TCP.

**R3 (Performance):** Demanded precomputed packet template (like TCP's PacketTemplate) for deterministic TX latency. Proposed UdpEndpoint class binding template + resources. Key insight: precompute 42-byte header, each send = 1 memcpy + 3 field updates.

**R14 (Architect):** Pushed for consistency with TcpTransport concept layer. Initially proposed UdpTransport concept. Flagged Reactor needs protocol-awareness for mixed TCP/UDP RX.

**R2 (Minimalist):** Counter-proposed just 2 functions (build_udp_packet + udp_checksum) in net_header.hpp. Argued YAGNI for classes.

**R6 (Maintainability):** Pointed out UDP code is scattered across 3 files with implicit duplication. Advocated unified packet building, regardless of abstraction level.

## Round 2 — Direct Rebuttals

**R5 vs R14:** "UdpTransport concept is a solution looking for a problem" — concept too thin (just send + port). R14 accepted, withdrew concept proposal.

**R3 vs R5:** "Even at 1K TX/s, eliminating variance matters more than average latency in trading" — precomputed template provides deterministic hot path.

**R3 vs R2:** "Passing 6 params every call is API nightmare" — convinced R2 that a lightweight binding class is justified.

**R2 vs R14:** "Don't pollute TCP's Reactor hot path" — proposed independent UDP RX path.

**R6:** Proposed `template <bool EnableUdp> class Reactor` — if constexpr eliminates runtime cost for TCP-only users.

## Round 3 — TX Abstraction Deep Dive

Converged on two-layer design matching TCP pattern:
- `UdpPacketTemplate` (pure packet building, no I/O) ↔ `PacketTemplate`
- `UdpSender` (binds resources + template) ↔ `TcpSession`

R2 raised naming concerns: "UdpEndpoint" sounds like Boost.Asio address pair. Settled on **UdpSender** — directional, no state-machine implication, consistent with MulticastReceiver.

## Round 4 — RX Path & Reactor

**R14:** Proposed 3 RX options: (A) template-parameterized Reactor, (B) independent UdpReceiver, (C) generic PacketDispatcher. Recommended A.

**R3:** Refined A — "check IP protocol byte before parse_packet, don't modify parse_packet". TCP path unchanged when EnableUdp=false.

**R2:** "Who needs unicast UDP RX now? No exchange use case." — Deferred RX to future. All agreed.

**R5:** Config should include optional rx_queue_id to not block future RX addition.

## Round 5 — Flow Steering & Details

**R3:** Flow Steering UDP = 20 lines (add protocol param to install_flow_rule). Do it now.
**R2:** Accepted — genuinely small change, not over-engineering.
**R5:** send() should return bool (not uint16_t), send_batch returns uint16_t.
**R5:** Use std::optional for rx_queue_id instead of magic 0xFFFF.
**R14:** UdpPacketTemplate should NOT hold port_id/tx_queue_id (consistency with PacketTemplate).

## Round 6-7 — Convergence

All roles confirmed no new arguments. Final positions documented.

---

## Final Design

### New Files
- `include/eph/dpdk/udp.hpp` — UdpConfig, UdpSender, build_udp_packet, UdpSegment
- `tests/test_udp.cpp` — Unit tests

### Modified Files
- `include/eph/dpdk/net_header.hpp` — Add UdpPacketTemplate, udp_checksum
- `include/eph/dpdk/flow_steering.hpp` — Add FlowProtocol enum, protocol parameter

### Core Types

```cpp
// === net_header.hpp additions ===

struct UdpPacketTemplate {
    alignas(64) uint8_t header_[42]{};  // Precomputed Eth(14)+IP(20)+UDP(8)
    uint16_t ip_id_{0};
    bool hw_cksum_{false};

    uint16_t fill(rte_mbuf* mbuf, const void* payload, uint16_t len) noexcept;
    rte_mbuf* build(rte_mempool* pool, const void* payload, uint16_t len) noexcept;
};

uint16_t udp_checksum(uint32_t src_ip_net, uint32_t dst_ip_net,
                      const void* udp_seg, uint16_t total_udp_len) noexcept;

// === udp.hpp ===

struct UdpConfig {
    uint32_t src_ip, dst_ip;                       // host order
    uint16_t src_port, dst_port;                    // host order
    rte_ether_addr src_mac, dst_mac;
    uint16_t port_id;
    uint16_t tx_queue_id;
    std::optional<uint16_t> rx_queue_id{};          // Reserved for future RX
    rte_mempool* pool;
    bool hw_cksum{false};
};

struct UdpSegment { const void* data; uint16_t len; };

class UdpSender {
    UdpPacketTemplate tmpl_;
    rte_mempool* pool_;
    uint16_t port_id_;
    uint16_t tx_queue_id_;
public:
    static std::expected<UdpSender, std::string> create(const UdpConfig& cfg) noexcept;
    bool send(const void* data, uint16_t len) noexcept;
    uint16_t send_batch(const UdpSegment* segs, uint16_t count) noexcept;
    const UdpPacketTemplate& packet_template() const noexcept;
};

rte_mbuf* build_udp_packet(rte_mempool* pool,
    const rte_ether_addr& src_mac, const rte_ether_addr& dst_mac,
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t src_port, uint16_t dst_port,
    const void* payload, uint16_t payload_len,
    bool hw_cksum = false) noexcept;

// === flow_steering.hpp modifications ===

enum class FlowProtocol : uint8_t { Tcp, Udp };

std::expected<FlowRule, std::string> install_flow_rule(
    uint16_t port_id, uint16_t queue_id,
    const net::ConnectionTuple& tuple,
    FlowProtocol proto = FlowProtocol::Tcp) noexcept;
```

### Checksum Strategy
- `hw_cksum = true`: NIC offload via RTE_MBUF_F_TX_UDP_CKSUM, validate NIC capability at create()
- `hw_cksum = false` (default): UDP checksum = 0 (legal for IPv4 per RFC 768), IP checksum software-computed

### TX Hot Path (UdpPacketTemplate::fill)
1. `rte_memcpy(data, header_, 42)` — copy precomputed header template
2. Update 3 dynamic fields: ip_total_length, ip_id++, udp_length
3. `rte_memcpy(data + 42, payload, len)` — payload
4. Set mbuf offload flags (hw) or compute IP checksum (sw)
5. Set mbuf->data_len = mbuf->pkt_len = 42 + len

### Deferred Work (Not Implemented Now)
- **Reactor UDP**: Future `template <ReactorProtocol> class Reactor` with if constexpr
- **Unicast UDP RX**: UdpSender gains poll_rx() when rx_queue_id is set
- **DNS refactor**: build_dns_packet uses UdpPacketTemplate (separate PR)
- **UdpTransport concept**: Not needed — UDP too simple for concept

### Resolved Disagreements

| Point | Resolution | Key Argument |
|-------|-----------|--------------|
| Functions vs classes | Two layers: UdpPacketTemplate + UdpSender | "Repeated 6 params is API nightmare" (R3) |
| UdpTransport concept | Not introduced | "Concept too thin" (R5) |
| Reactor extension | Deferred with template extension point | "No current use case" (R2) |
| Flow Steering UDP | Implement now | "20 lines of changes" (R3) |
| Checksum strategy | hw=NIC offload, sw=skip (cksum=0) | "IPv4 UDP checksum optional per RFC 768" |
| DNS refactor | Separate PR | "Reduce regression risk" (R2) |
| Naming | UdpSender (not UdpEndpoint/UdpSession) | "Consistent with MulticastReceiver" (R6) |
| send() return | bool (single), uint16_t (batch) | "rte_eth_tx_burst returns pkt count" (R5) |

### Open Decisions for User
1. **hw_cksum default**: false recommended (safe default) unless target NIC known
2. **UdpPacketTemplate placement**: net_header.hpp recommended (parallel with TCP PacketTemplate)
