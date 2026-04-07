# Discussion Record

## Context
- 时间：2026-04-07
- 用户原始需求：如何扩展 eph-dpdk Reactor 支持 TCP + UDP 共享同一个 RX 队列？当前 Reactor (reactor.hpp) 硬编码 TCP-only 分发，需要在不退化 TCP 热路径性能的前提下支持 UDP 报文回调。
- 复杂度评估：中
- 讨论轮数：4 轮（第 3-4 轮收敛）
- 参与角色：R3 性能狂热者, R14 架构师, R2 极简主义者, R7 用户代言人, R1 风险卫士

## 内容摘要

R3 确立了 `template <bool EnableUdp> class Reactor` + `if constexpr` 为唯一满足"TCP 零退化"约束的方案。R14 定稿四元组匹配机制（与 TCP 一致的 hash_tuple 复用）。R1/R2 推动将 `parse_udp_packet`/`ParsedUdpPacket` 从 multicast.hpp 搬到 net_header.hpp，避免 reactor.hpp 依赖 multicast.hpp。全员同意 UDP 条目只能在 start() 前注册（不支持运行时增删），MulticastReceiver 保持独立不合并。

## Final Design

### Template
```cpp
template <bool EnableUdp = false>
class Reactor { ... };
```

### UDP entry
```cpp
using UdpDataCallback = std::function<void(
    const uint8_t* payload, uint16_t len, size_t udp_id)>;

struct UdpReactorEntry {
    net::ConnectionTuple tuple{};
    UdpDataCallback on_data{};
};
```

### Registration
```cpp
auto add_udp(const net::ConnectionTuple& tuple, UdpDataCallback on_data)
    requires (EnableUdp)
    -> std::expected<size_t, std::string>;
```

### Hot path
```cpp
if constexpr (EnableUdp) {
    // Read IP protocol byte (1 byte, ~3ns)
    if (ip->next_proto_id == kIpProtoUdp) {
        dispatch_udp_packet(pkts[i]);
        continue;
    }
}
// TCP path unchanged
```

### Prerequisites
- Move ParsedUdpPacket + parse_udp_packet from multicast.hpp to net_header.hpp
- multicast.hpp uses them from net_header.hpp (no behavior change)

### Resolved
| Point | Resolution |
|-------|-----------|
| Template param | bool (not enum) |
| Match granularity | 4-tuple (not 2-tuple) |
| Runtime add/remove | Not supported (register before start) |
| Parse split | No split (1-byte read acceptable) |
| UDP memory in Reactor<false> | Accept ~1KB unused (cold data) |
| parse_udp_packet location | Move to net_header.hpp |
| MulticastReceiver | Independent, not merged |
