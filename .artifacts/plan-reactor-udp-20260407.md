# Plan: Reactor UDP Support + 技术遗留清理

> 通过 4 项精准改动消除 TCP 绑定遗留：新增 parse_ip_header、搬移 ParsedUdpPacket、模板化 Reactor、修正注释。

创建时间：2026-04-07
状态：已确认
设计参考：
- .artifacts/discuss-20260407-reactor-udp.md
- .artifacts/discuss-20260407-udp-redesign.md

---

## 定位与边界

**目标**：让 Reactor 支持 TCP+UDP 共享 RX 队列，同时清理阻碍 UDP 支持的技术遗留。
**用户**：需要在同一 DPDK RX 队列上同时接收 TCP 和 UDP 单播报文的交易系统开发者。

**In scope**：
- 新增 `ParsedIpHeader` + `parse_ip_header()`（L2/L3 公共解析）
- 搬移 `ParsedUdpPacket` + `parse_udp_packet()` 到 `net_header.hpp`
- 模板化 `template <bool EnableUdp = false> class Reactor`
- `ConnectionTuple` 注释修正

**Out of scope**：
- MulticastReceiver 不改（独立 RX 线程 + MAC filter 不合并）
- `PacketTemplate` 不改（TCP 和 UDP 模板并列是正确设计）
- `TcpTransport` concept 不改（TCP 确实需要专用 concept）
- `types.hpp` 不改（UDP 不需要 Transport alias）
- `parse_packet` 公共 API 行为不变（内部可重构为调用 parse_ip_header）

---

## 架构设计

### 目标分层

```
net_header.hpp (L2/L3/L4 解析与构建)
├── parse_ip_header()    → ParsedIpHeader (新增)
├── parse_packet()       → ParsedPacket (TCP，内部重构)
├── parse_udp_packet()   → ParsedUdpPacket (从 multicast 搬来)
├── PacketTemplate       (TCP 模板，不变)
└── UdpPacketTemplate    (UDP 模板，不变)

reactor.hpp (协议感知 RX 分发)
└── template <bool EnableUdp> class Reactor
    ├── TCP 条目 (ReactorEntry，不变)
    ├── UDP 条目 (UdpReactorEntry，仅 EnableUdp=true)
    └── 热路径: parse_ip_header → proto 分流 → TCP/UDP 各自路径
```

### 核心抽象

**ParsedIpHeader — L2+L3 公共解析结果**

```cpp
namespace eph::dpdk::net {

/// @brief Minimal L2+L3 parse result for protocol dispatch.
/// Extracts Ethernet + IPv4 headers and IP protocol number
/// without parsing L4 (TCP/UDP). Used by Reactor to determine
/// protocol before committing to a full L4 parse.
struct ParsedIpHeader {
    const rte_ether_hdr* eth{nullptr};
    const rte_ipv4_hdr*  ip{nullptr};
    uint8_t ihl{0};    ///< IP header length in bytes (typically 20)
    uint8_t proto{0};  ///< IP protocol number (kIpProtoTcp=6 / kIpProtoUdp=17)

    [[nodiscard]] explicit operator bool() const noexcept { return ip != nullptr; }

    [[nodiscard]] uint32_t src_ip() const noexcept {
        return ip ? ntoh32(ip->src_addr) : 0;
    }
    [[nodiscard]] uint32_t dst_ip() const noexcept {
        return ip ? ntoh32(ip->dst_addr) : 0;
    }
};

[[nodiscard]] inline ParsedIpHeader
parse_ip_header(const rte_mbuf* mbuf) noexcept;

} // namespace eph::dpdk::net
```

**UdpReactorEntry — Reactor 的 UDP 条目**

```cpp
namespace eph::dpdk {

/// Callback for UDP payload delivery in Reactor context.
using UdpReactorCallback =
    std::function<void(const uint8_t* data, uint16_t len, size_t udp_id)>;

/// Per-UDP-entry in the Reactor's fixed-size table.
struct UdpReactorEntry {
    net::ConnectionTuple tuple{};
    UdpReactorCallback on_data{};
};

} // namespace eph::dpdk
```

### 数据流（Reactor 热路径）

```
rte_eth_rx_burst(pkts[], 32)
  │
  ▼ for each pkt
parse_ip_header(pkt) → ParsedIpHeader { eth, ip, proto }
  │
  ├─ if constexpr (EnableUdp) && proto == UDP
  │    parse_udp_packet(pkt) → ParsedUdpPacket
  │    hash + 线性扫描 udp_entries_ (≤8)
  │    匹配 → callback(payload, len, udp_id)
  │    free mbuf
  │    continue
  │
  ├─ proto == TCP (或 EnableUdp=false 时所有包)
  │    parse_tcp_from_ip(pkt, ip_hdr) → ParsedPacket
  │    hash + 线性扫描 entries_ (≤16)
  │    匹配 → session->process_rx + flush_pending_ack
  │
  └─ 不匹配 → free mbuf
```

---

## 接口设计

### 新增公共 API

```cpp
// net_header.hpp
namespace eph::dpdk::net {
    struct ParsedIpHeader { ... };
    [[nodiscard]] inline ParsedIpHeader parse_ip_header(const rte_mbuf*) noexcept;
}

// reactor.hpp
namespace eph::dpdk {
    template <bool EnableUdp = false>
    class Reactor {
        // 现有 TCP API（不变）
        std::expected<size_t, std::string>
            add_connection(TcpSession<>* session, ReactorDataCallback on_data);

        // 新增 UDP API（仅 EnableUdp=true 可用）
        std::expected<size_t, std::string>
            add_udp(const net::ConnectionTuple& tuple, UdpReactorCallback on_data)
            requires (EnableUdp);

        void set_udp_active(size_t udp_id, bool active) noexcept
            requires (EnableUdp);
    };
}
```

### 错误体系

- `add_udp` → `std::expected<size_t, std::string>`（与 `add_connection` 一致）
- `set_udp_active` → `void`（out-of-range 静默忽略 + WARN 日志，与 `mark_disconnected` 一致）

---

## 编码规范

| 维度 | 规范 |
|------|------|
| 命名 | `parse_ip_header`（与 `parse_packet`/`parse_udp_packet` 并列）、`UdpReactorEntry`/`UdpReactorCallback` |
| 日志 | logger name `"dpdk.reactor"`（复用现有）、UDP 分发用 TRACE 级别 |
| 注释 | `ParsedIpHeader` 和 `parse_ip_header` 需 Doxygen 注释说明与 `parse_packet` 的关系 |
| 模板 | `template <bool EnableUdp = false>` 而非 enum（只有两个状态） |
| 约束 | `requires (EnableUdp)` 限制 UDP API 可见性 |

---

## 实施计划

### 阶段 1: net_header.hpp — ParsedIpHeader + parse_ip_header + 搬移 ParsedUdpPacket

**交付物**：
- `net_header.hpp` 新增 `ParsedIpHeader` 结构体 + `parse_ip_header()` 函数
- `net_header.hpp` 搬入 `ParsedUdpPacket` + `parse_udp_packet()`（从 `multicast.hpp`）
- `multicast.hpp` 删除原定义，加 `using net::ParsedUdpPacket; using net::parse_udp_packet;`
- `ConnectionTuple` 注释修正为 "network connection"
- `parse_packet` 内部重构为调用 `parse_ip_header`（行为不变）
- `parse_udp_packet` 内部重构为调用 `parse_ip_header`（行为不变）
- 测试：`test_net_header.cpp` 新增 `parse_ip_header` 测试
- 测试：`test_multicast.cpp` 验证搬移后无回归

**具体实现**：

1. **`parse_ip_header(mbuf)`**：放在 `parse_packet` 之前
   - 检查 pkt_len >= kEtherHeaderLen + kIpv4HeaderLen
   - 验证 ether_type == IPv4, version == 4, IHL >= 5
   - 返回 `ParsedIpHeader { eth, ip, ihl, proto }`

2. **重构 `parse_packet`**：提取前半段为 `parse_ip_header` 调用
   ```cpp
   ParsedPacket parse_packet(const rte_mbuf* mbuf) noexcept {
       auto ip_hdr = parse_ip_header(mbuf);
       if (!ip_hdr || ip_hdr.proto != kIpProtoTcp) return {};
       // 从 ip_hdr 继续解析 TCP header（现有逻辑）
       ...
   }
   ```

3. **搬移 `ParsedUdpPacket`**：从 `multicast.hpp` 移到 `net_header.hpp`
   - 放在 `ParsedPacket` 之后（L4 解析结果并列）
   - namespace 改为 `eph::dpdk::net`
   - 同样重构为调用 `parse_ip_header`

4. **`multicast.hpp` 兼容 alias**：
   ```cpp
   using net::ParsedUdpPacket;
   using net::parse_udp_packet;
   // 删除 kMinUdpPacketLen（搬到 net_header.hpp）
   ```

**验收标准**：
- `xmake build && xmake run test_net_header` — 全部通过（含新增测试）
- `xmake run test_multicast` — 全部通过（零回归）
- `xmake run test_udp` — 全部通过（零回归）
- `xmake build` — 全项目编译无 warning

**预估**：~200 行新增/搬移 + ~100 行测试

---

### 阶段 2: reactor.hpp — 模板化 + UDP 条目 + 分发

**交付物**：
- `reactor.hpp` 模板化为 `template <bool EnableUdp = false>`
- 新增 `UdpReactorEntry`、`UdpReactorCallback`
- 新增 `add_udp()` + `set_udp_active()`
- `rx_loop` 使用 `parse_ip_header` → proto 分流 → TCP/UDP 各自路径
- 新增 `dispatch_udp_packet()` 内部方法
- 测试：`test_reactor.cpp` 新增模板化和 UDP 条目测试

**具体实现**：

1. **模板化 Reactor**
   ```cpp
   template <bool EnableUdp = false>
   class Reactor { ... };
   ```
   - 现有 `Reactor` 用户改为 `Reactor<>` 或 `Reactor<false>`（向后兼容）

2. **UDP 条目存储**（无论 EnableUdp 值，始终声明但仅 true 时使用）
   ```cpp
   static constexpr size_t kReactorMaxUdpEntries = 8;
   std::array<UdpReactorEntry, kReactorMaxUdpEntries> udp_entries_{};
   std::array<uint64_t, kReactorMaxUdpEntries> udp_hashes_{};
   std::array<std::atomic<bool>, kReactorMaxUdpEntries> udp_active_{};
   std::atomic<size_t> udp_count_{0};
   ```

3. **`add_udp()` 实现**
   ```cpp
   [[nodiscard]] std::expected<size_t, std::string>
   add_udp(const net::ConnectionTuple& tuple, UdpReactorCallback on_data)
       requires (EnableUdp)
   {
       // start 前调用检查
       // count 上限检查
       // 存储 tuple + hash + callback + active=true
       // 返回 udp_id
   }
   ```

4. **`rx_loop` 修改**：
   ```cpp
   for (uint16_t i = 0; i < nb_rx; ++i) {
       if constexpr (EnableUdp) {
           auto ip_hdr = net::parse_ip_header(pkts[i]);
           if (ip_hdr && ip_hdr.proto == net::kIpProtoUdp) {
               dispatch_udp_packet(pkts[i]);
               continue;
           }
       }
       // 现有 TCP 路径不变
       auto parsed = net::parse_packet(pkts[i]);
       if (!parsed.tcp) { rte_pktmbuf_free(pkts[i]); continue; }
       // ... TCP 匹配+分发 ...
   }
   ```

5. **`dispatch_udp_packet()`**：
   - `parse_udp_packet(mbuf)` 获取完整 UDP 解析
   - 构建 `ConnectionTuple` + hash
   - 线性扫描 `udp_entries_`（hash 快速拒绝 + 四元组反转匹配）
   - 匹配成功 → 回调 `on_data(payload, payload_len, udp_id)` → free mbuf
   - 无匹配 → free mbuf

6. **`set_udp_active()`**：
   ```cpp
   void set_udp_active(size_t udp_id, bool active) noexcept
       requires (EnableUdp)
   {
       // bounds check + atomic store
   }
   ```

**测试**（`test_reactor.cpp` 扩展）：
- `Reactor_DefaultIsNotUdp` — `Reactor<>` 等价 `Reactor<false>`
- `Reactor_AddUdpRequiresTemplate` — 编译期验证 `Reactor<false>` 无 `add_udp`
- `ReactorUdp_AddUdpBeforeStart` — 有效注册返回 udp_id
- `ReactorUdp_AddUdpAfterStartFails` — 运行时拒绝
- `ReactorUdp_AddUdpFull` — 达到 kReactorMaxUdpEntries 后返回 error
- `ReactorUdp_SetUdpActive` — enable/disable 切换
- `UdpReactorEntry_Defaults` — 默认值正确
- `UdpReactorCallback_Type` — 签名检查

**验收标准**：
- `xmake build && xmake run test_reactor` — 全部通过（含新增测试）
- 现有 TCP Reactor 测试零回归
- `xmake build` — 全项目编译无 warning
- `Reactor<false>` 实例化不包含任何 UDP 方法（编译期验证）

**预估**：~200 行新增 + ~100 行测试

---

### 阶段 3: 集成验证 + 文档更新

**交付物**：
- `dpdk.hpp` 伞头文件确认包含 `udp.hpp`（已有）
- 全项目 `xmake build` 通过
- 全部相关测试通过（test_net_header + test_multicast + test_udp + test_reactor + test_tcp + test_connector）
- `reactor.hpp` 文件头注释更新（提及 UDP 支持）

**验收标准**：
- 全项目编译零 warning
- 所有测试通过
- 无循环依赖引入

**预估**：~30 行

---

## 关键决策记录

### D-1: 并列 vs 统一协议抽象
- **问题**：TCP/UDP 解析和分发应该统一为一个协议无关层，还是保持并列
- **选项**：A. 统一 ParsedPacket（含 union/variant） / B. 并列 ParsedPacket + ParsedUdpPacket
- **决策**：B（并列）
- **理由**：TCP 有状态机/重排序/ACK，UDP 无状态。统一要么太薄（无价值）要么引入 type-erasure 开销。并列在编译期确定协议路径，零运行时分支。
- **验收标准**：`ParsedPacket` 和 `ParsedUdpPacket` 作为独立类型存在于 `net_header.hpp`

### D-2: parse_ip_header 是否必要
- **问题**：Reactor 协议分流是直接读 1 字节 protocol 还是用公共 parse 函数
- **选项**：A. 直接读字节 / B. parse_ip_header() 函数
- **决策**：B
- **理由**：(1) 消除 Reactor 中 IP 头重复解析（parse_ip_header 一次，结果传给后续 TCP/UDP parse）；(2) parse_packet/parse_udp_packet 可内部重构调用它，减少代码重复；(3) inline 后零开销
- **验收标准**：Reactor rx_loop 调用 parse_ip_header 一次，后续 TCP/UDP 路径不重新解析 IP 头

### D-3: UDP 回调签名
- **问题**：传 ParsedUdpPacket 还是裸 (payload, len)
- **决策**：裸 `(payload, len, udp_id)`（与 TCP ReactorDataCallback 一致）
- **理由**：用户注册时已知固定对端四元组，不需要每包重复读取源地址
- **验收标准**：`UdpReactorCallback` 签名为 `void(const uint8_t*, uint16_t, size_t)`

### D-4: Reactor 模板参数
- **问题**：enum 还是 bool
- **决策**：`bool`（`template <bool EnableUdp = false>`）
- **理由**：只有两个状态，enum 过度抽象
- **验收标准**：`Reactor<false>` 编译产物与未模板化前的 Reactor 相同
