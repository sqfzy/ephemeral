# Discussion Record — Ideal eph-dpdk Architecture

## Context
- 时间：2026-04-07
- 用户原始需求：假如一切重来，不考虑兼容性。你会如何设计最佳的eph-dpdk代码架构？
- 复杂度评估：极高
- 讨论轮数：5 轮
- 参与角色：R8 激进创新者, R5 第一性原理者, R3 性能狂热者, R14 架构师, R6 维护性倡导者

## 内容摘要

R8 提出 5 文件拆分方案被 R5 缩减为 3 文件（按变更频率分组：core/parse/template），避免碎片化。R8 提出的分层 parse API（parse_ip_header → parse_tcp/udp_from_ip）获得全员支持，可消除 Reactor 中的 IP 头重复解析。R3 否决了运行时 UDP 分支，确认模板化 Reactor 是从零设计也会得出的结论。最终共识：当前架构方向基本正确，主要改进点在 net_header.hpp 拆分和 parse 分层——说明增量演进已接近理想状态。

---

## Ideal Architecture

```
Layer 0: eal.hpp, platform.hpp
Layer 1: packet_core.hpp (~250L), packet_parse.hpp (~300L), packet_template.hpp (~400L), net_header.hpp (umbrella)
Layer 2: tcp.hpp, udp.hpp, arp.hpp, dns.hpp, multicast.hpp
Layer 3: reactor.hpp (template<bool>), connector.hpp (+bind_udp), flow_steering.hpp, types.hpp
```

## Key Improvements vs Current

1. **Split net_header.hpp** into 3 files by change frequency (core=stable, parse/template=evolving)
2. **Layered parse API**: parse_ip_header → parse_tcp_from_ip / parse_udp_from_ip (eliminates redundant IP parsing)
3. **connector.hpp += bind_udp()**: UDP one-call setup parallel to TCP connect()
4. Current Reactor template design, UdpPacketTemplate, UdpSender are already ideal — no changes needed

## Resolved Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Split granularity | 3 files (not 5) | <100 line files not worth separating |
| Directory vs prefix | Prefix `packet_*.hpp` | Keep flat layout convention |
| Internal concepts | None | Only 2 implementations, not worth abstracting |
| Reactor design | template<bool EnableUdp> | Compile-time elimination is only zero-overhead option |
| ParsedIpHeader size | Keep minimal (no data/pkt_len) | Don't store what caller already has |
| parse_*_from_ip signature | (mbuf, ip_hdr) | mbuf available at callsite |
