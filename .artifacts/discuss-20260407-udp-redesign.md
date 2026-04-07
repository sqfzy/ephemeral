# Discussion Record

## Context
- 时间：2026-04-07
- 用户原始需求：目前的技术遗留似乎不利于UDP的支持，如果让你重新设计，一步到位，应该如何设计才是最佳实践？
- 复杂度评估：高
- 讨论轮数：3 轮
- 参与角色：R8 激进创新者, R4 实用主义者, R3 性能狂热者, R14 架构师, R9 成本审计者

## 内容摘要

R8 提出推倒重来的协议无关架构，被 R4/R9 用成本分析否决（2000 行 vs 460 行，收益比 1:4）。R14 提出关键洞察："TCP/UDP 本质不同，正确的架构是并列而非统一"。R8 缩小范围到 `parse_ip_header`（提取 L2/L3 公共解析），R3 验证 inline 后零退化。最终共识：4 项精准改动（新增 parse_ip_header、搬移 parse_udp_packet、模板化 Reactor、修正注释）消除全部技术遗留，~460 行，零公共 API 破坏。

---

## Key Insight

"TCP 和 UDP 的差异不是实现细节，而是本质不同。TCP 有状态机/重排序/ACK 需要 session，UDP 无状态只需 callback。试图用一个统一的协议无关层抽象掉这些差异，要么太薄（没有价值），要么太厚（引入 type-erasure 开销）。正确的架构不是'统一'，而是'并列'。"

## Target Architecture

```
net_header.hpp  (L2/L3/L4 parse & build)
├── parse_ip_header()   → ParsedIpHeader (NEW)
├── parse_packet()      → ParsedPacket (TCP, refactored internally)
├── parse_udp_packet()  → ParsedUdpPacket (MOVED from multicast.hpp)
├── PacketTemplate      (TCP template)
└── UdpPacketTemplate   (UDP template)

reactor.hpp  (protocol-aware RX dispatch)
└── template <bool EnableUdp> class Reactor
    ├── TCP entries (existing ReactorEntry)
    ├── UDP entries (new UdpReactorEntry, only when EnableUdp=true)
    └── Hot path: parse_ip_header → proto switch → TCP/UDP paths

udp.hpp  (UDP TX, already implemented)
multicast.hpp  (multicast RX, independent)
tcp.hpp  (TCP state machine, unchanged)
```

## 4 Changes Required

1. **Add** `ParsedIpHeader` + `parse_ip_header()` to net_header.hpp (~50 lines)
2. **Move** `ParsedUdpPacket` + `parse_udp_packet()` from multicast.hpp to net_header.hpp (~80 lines moved)
3. **Template** Reactor as `template <bool EnableUdp = false>` (~150 lines new)
4. **Fix** `ConnectionTuple` comment from "TCP connection" to "network connection"

Total: ~460 lines changed, zero public API breakage
