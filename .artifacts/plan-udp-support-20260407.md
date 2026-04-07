# Plan: eph-dpdk UDP Support Layer

> 新增 UdpPacketTemplate + UdpSender 两层 TX 抽象，扩展 Flow Steering 支持 UDP 规则。

创建时间：2026-04-07
状态：已完成
设计参考：.artifacts/discuss-20260407-dpdk-udp.md

---

## 定位与边界

**目标**：为 eph-dpdk 提供高性能 UDP 单播发送能力，预计算报文模板实现确定性低延迟 TX。
**用户**：需要通过 DPDK 发送 UDP 单播报文的交易系统开发者。

**In scope**：
- `UdpPacketTemplate`：预计算 42 字节头部模板，零分配热路径 fill/build
- `UdpSender`：绑定资源（pool, port, queue）的 TX 句柄，提供 send/send_batch
- `udp_checksum`：UDP 校验和工具函数
- `build_udp_packet`：一次性便捷函数（不需要 UdpSender 实例）
- Flow Steering 扩展：`install_flow_rule` 支持 UDP 四元组规则
- 单元测试：`test_udp.cpp`、`test_flow_steering.cpp` 扩展

**Out of scope**：
- UDP RX 路径（无当前 use case，UdpConfig 中 rx_queue_id 预留）
- Reactor UDP 扩展（推迟，设计留 template 扩展点）
- UdpTransport concept（UDP 太简单，不需要 concept 抽象）
- DNS 重构（独立 PR，降低回归风险）
- IP 分片/重组
- 可靠性/重传机制

---

## 架构设计

### 模块划分

| 模块 | 文件 | 职责 | 操作 |
|------|------|------|------|
| 报文模板层 | `net_header.hpp` | `UdpPacketTemplate` + `udp_checksum` | 修改 |
| 资源绑定层 | `udp.hpp`（新） | `UdpConfig` + `UdpSender` + `build_udp_packet` | 新增 |
| 硬件分发层 | `flow_steering.hpp` | `FlowProtocol` 枚举 + `install_flow_rule` 协议参数 | 修改 |
| 伞头文件 | `dpdk.hpp` | 新增 `#include "eph/dpdk/udp.hpp"` | 修改 |
| 测试 | `test_udp.cpp`（新）、`test_flow_steering.cpp` | 单元测试 | 新增+修改 |

### 两层设计（与 TCP 对应）

```
TCP 对应关系：
  PacketTemplate  ←→  UdpPacketTemplate   （报文模板，无 I/O）
  TcpSession      ←→  UdpSender           （资源绑定 + I/O）
  TcpConfig       ←→  UdpConfig           （配置）
```

### 数据流（TX 热路径）

```
用户调用 UdpSender::send(payload, len)
  → UdpPacketTemplate::fill(mbuf, payload, len)
    → rte_memcpy(data, header_, 42)           // 复制预计算头部
    → 更新 ip_total_length, ip_id++, udp_length  // 3 个动态字段
    → rte_memcpy(data + 42, payload, len)     // payload
    → 设置 mbuf offload flags 或 IP checksum
  → rte_eth_tx_burst(port, queue, &mbuf, 1)
```

---

## 接口设计

### 公共 API

#### net_header.hpp 新增

```cpp
// ─── UDP 报文头长度常量（已有） ───
// inline constexpr uint16_t kUdpHeaderLen = 8;
// inline constexpr uint16_t kEtherHeaderLen + kIpv4HeaderLen + kUdpHeaderLen = 42

/// 组合头长：Eth(14) + IP(20) + UDP(8) = 42 字节
inline constexpr uint16_t kUdpAllHeadersLen = kEtherHeaderLen + kIpv4HeaderLen + kUdpHeaderLen;

/// UDP checksum（与 tcp_checksum 并列）
/// 用于测试验证和需要 software checksum 的场景
[[nodiscard]] inline uint16_t
udp_checksum(uint32_t src_ip_net, uint32_t dst_ip_net,
             const void* udp_seg, uint16_t total_udp_len) noexcept;

/// 预计算 Eth+IP+UDP 头部模板
/// 构造时将所有静态字段（MAC, IP, port）预转换为 network byte order
/// 存入 42 字节 header_ 数组。每次发送只更新 3 个动态字段。
///
/// @note 非线程安全（ip_id_ 递增）。每个 TX 线程使用独立实例。
struct UdpPacketTemplate {
    alignas(64) uint8_t header_[kUdpAllHeadersLen]{};
    uint16_t ip_id_{0};
    bool hw_cksum_{false};

    /// 从配置初始化预计算头部
    /// @param src_mac, dst_mac  以太网地址
    /// @param src_ip, dst_ip    IPv4 地址（host order）
    /// @param src_port, dst_port UDP 端口（host order）
    /// @param hw_cksum          是否启用 NIC checksum offload
    void init(const rte_ether_addr& src_mac, const rte_ether_addr& dst_mac,
              uint32_t src_ip, uint32_t dst_ip,
              uint16_t src_port, uint16_t dst_port,
              bool hw_cksum = false) noexcept;

    /// 热路径：填入预分配 mbuf。调用者负责 mbuf 分配。
    /// @return 写入的总字节数（42 + payload_len），0 表示失败
    uint16_t fill(rte_mbuf* mbuf,
                  const void* payload, uint16_t payload_len) noexcept;

    /// 便捷路径：从 pool 分配 mbuf 并填充
    /// @return 填充好的 mbuf，nullptr 表示分配失败
    rte_mbuf* build(rte_mempool* pool,
                    const void* payload, uint16_t payload_len) noexcept;
};
```

#### udp.hpp（新文件）

```cpp
#pragma once
#include "eph/dpdk/net_header.hpp"

namespace eph::dpdk {

/// UDP 发送端配置
struct UdpConfig {
    uint32_t src_ip{};              ///< 源 IP（host order）
    uint32_t dst_ip{};              ///< 目标 IP（host order）
    uint16_t src_port{};            ///< 源端口（host order）
    uint16_t dst_port{};            ///< 目标端口（host order）
    rte_ether_addr src_mac{};       ///< 源 MAC
    rte_ether_addr dst_mac{};       ///< 目标 MAC（通常为 gateway MAC）
    uint16_t port_id{};             ///< DPDK port ID
    uint16_t tx_queue_id{};         ///< TX queue index
    std::optional<uint16_t> rx_queue_id{};  ///< 预留，未来 RX 扩展
    rte_mempool* pool{nullptr};     ///< mbuf pool
    bool hw_cksum{false};           ///< NIC checksum offload

    /// 验证配置有效性
    [[nodiscard]] constexpr std::string_view validate() const noexcept;
};

/// UDP 发送段描述符（用于 batch send）
struct UdpSegment {
    const void* data{nullptr};
    uint16_t len{0};
};

/// UDP 发送统计
struct UdpSenderStats {
    uint64_t tx_packets{0};
    uint64_t tx_bytes{0};
    uint64_t tx_errors{0};     ///< rte_eth_tx_burst 返回 0 或 mbuf 分配失败
};

/// 绑定资源的 UDP TX 句柄
///
/// 持有预计算的报文模板 + DPDK 资源引用。
/// 提供 send() 单包发送和 send_batch() 批量发送。
///
/// @note 非线程安全。每个 TX 线程使用独立 UdpSender 实例。
class UdpSender {
    UdpPacketTemplate tmpl_;
    rte_mempool* pool_;
    uint16_t port_id_;
    uint16_t tx_queue_id_;
    UdpSenderStats stats_{};

public:
    /// 工厂函数：验证配置 + 初始化模板
    /// 如果 hw_cksum=true，验证 NIC 支持 RTE_ETH_TX_OFFLOAD_UDP_CKSUM
    [[nodiscard]] static std::expected<UdpSender, std::string>
    create(const UdpConfig& cfg) noexcept;

    /// 发送单个 UDP 报文
    /// @return true 如果 rte_eth_tx_burst 成功发送
    bool send(const void* data, uint16_t len) noexcept;

    /// 批量发送多个 UDP 报文
    /// @return 实际发送的包数（可能 < count）
    uint16_t send_batch(const UdpSegment* segs, uint16_t count) noexcept;

    /// 获取统计信息（非原子，快照语义）
    [[nodiscard]] const UdpSenderStats& stats() const noexcept;

    /// 获取内部报文模板（高级用法：直接操作 mbuf）
    [[nodiscard]] const net::UdpPacketTemplate& packet_template() const noexcept;

    /// Human-readable dump
    [[nodiscard]] std::string dump() const;

    /// JSON stats
    [[nodiscard]] std::string stats_json() const;
};

/// 一次性便捷函数：构建单个 UDP 报文（不需要 UdpSender 实例）
/// 适用于低频、不固定目标的场景（如 DNS 查询）
[[nodiscard]] rte_mbuf* build_udp_packet(
    rte_mempool* pool,
    const rte_ether_addr& src_mac, const rte_ether_addr& dst_mac,
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t src_port, uint16_t dst_port,
    const void* payload, uint16_t payload_len,
    bool hw_cksum = false) noexcept;

} // namespace eph::dpdk
```

#### flow_steering.hpp 修改

```cpp
/// 流规则协议类型
enum class FlowProtocol : uint8_t { Tcp, Udp };

/// Human-readable name
[[nodiscard]] constexpr std::string_view flow_protocol_name(FlowProtocol p) noexcept;

/// 安装流规则（新增 proto 参数，默认 Tcp 保持向后兼容）
[[nodiscard]] inline std::expected<FlowRule, std::string>
install_flow_rule(uint16_t port_id, uint16_t queue_id,
                  const net::ConnectionTuple& tuple,
                  FlowProtocol proto = FlowProtocol::Tcp) noexcept;
```

### 错误体系

- `UdpSender::create` → `std::expected<UdpSender, std::string>`（与 `TcpSession` 一致）
- `UdpConfig::validate` → `std::string_view`（空=有效，与 `PacketTemplate::validate` 一致）
- `UdpSender::send` → `bool`（热路径，无错误描述）
- `UdpSender::send_batch` → `uint16_t`（返回实际发送数）

---

## 编码规范

| 维度 | 规范 |
|------|------|
| 命名 | 遵循现有风格：`kConstant`、`method_name`、`member_`、`ClassName` |
| 日志 | spdlog，logger name `"dpdk.udp"`，与 `"dpdk.flow"` / `"dpdk.tcp"` 并列 |
| 错误处理 | `std::expected` 用于工厂函数，`bool`/`uint16_t` 用于热路径 |
| 注释 | Doxygen 风格 `///`，参考 `PacketTemplate` 和 `FlowRule` 的注释密度 |
| 头文件守卫 | `#pragma once`（项目统一风格） |
| 对齐 | 热路径结构体 `alignas(64)` 避免 false sharing |

---

## 实施计划

### 阶段 1: net_header.hpp — UdpPacketTemplate + udp_checksum

**交付物**：
- `net_header.hpp` 新增 `kUdpAllHeadersLen` 常量
- `net_header.hpp` 新增 `udp_checksum()` 函数
- `net_header.hpp` 新增 `UdpPacketTemplate` 结构体（`init`, `fill`, `build`）
- `test_net_header.cpp` 新增测试用例

**具体实现**：

1. **`kUdpAllHeadersLen`**：放在 `kAllHeadersLen` 之后

2. **`udp_checksum()`**：放在 `tcp_checksum()` 之后，复用 `pseudo_header_sum`
   ```cpp
   [[nodiscard]] inline uint16_t
   udp_checksum(uint32_t src_ip_net, uint32_t dst_ip_net,
                const void* udp_seg, uint16_t total_udp_len) noexcept {
       uint32_t sum = pseudo_header_sum(src_ip_net, dst_ip_net, kIpProtoUdp, total_udp_len);
       // 与 tcp_checksum 相同的 16-bit word 累加逻辑
       ...
   }
   ```

3. **`UdpPacketTemplate`**：放在 `PacketTemplate` 之后（约 line 526 后）
   - `init()`：预填 header_[42] 中所有静态字段
     - Eth: dst_mac, src_mac, ether_type=0x0800
     - IP: version_ihl=0x45, ttl=64, proto=17, src_addr, dst_addr, DF flag
     - UDP: src_port, dst_port
     - 动态字段（ip_total_length, ip_id, udp_length）设为 0 占位
   - `fill(mbuf, payload, len)`：
     1. `rte_pktmbuf_reset(mbuf)`
     2. `rte_pktmbuf_append(mbuf, 42 + len)`
     3. `std::memcpy(data, header_, 42)` — 复制预计算头部
     4. 更新 `ip->total_length = hton16(20 + 8 + len)`
     5. 更新 `ip->packet_id = hton16(ip_id_++)`
     6. 更新 `udp->length = hton16(8 + len)`
     7. `std::memcpy(data + 42, payload, len)` — payload
     8. if hw_cksum: 设 ol_flags, ip checksum=0, udp checksum=pseudo-header
        else: ip checksum = software, udp checksum = 0
     9. 设 mbuf->data_len = mbuf->pkt_len = 42 + len
   - `build(pool, payload, len)`：alloc + fill

**测试**（`test_net_header.cpp` 新增）：
- `static_assert(kUdpAllHeadersLen == 42)`
- `UdpChecksum_ZeroPayload` — 空 payload 的 checksum
- `UdpChecksum_KnownVector` — RFC 768 测试向量
- `UdpChecksum_MatchesTcpPattern` — 与 pseudo_header_sum + manual sum 一致
- `UdpPacketTemplate_Init` — 验证 header_ 中各字段正确
- `UdpPacketTemplate_Fill_CorrectHeaders` — 解析 fill 输出验证所有字段
- `UdpPacketTemplate_Fill_PayloadCopied` — payload 内容正确
- `UdpPacketTemplate_Fill_IpIdIncrements` — 连续 fill 后 ip_id 递增
- `UdpPacketTemplate_Build_AllocAndFill` — build 返回有效 mbuf
- `UdpPacketTemplate_Build_NullPool` — pool=null 返回 nullptr
- `UdpPacketTemplate_HwCksum_OffloadFlags` — hw_cksum=true 时 ol_flags 正确
- `UdpPacketTemplate_SwCksum_UdpChecksumZero` — hw_cksum=false 时 udp checksum=0
- `UdpPacketTemplate_SwCksum_IpChecksumValid` — hw_cksum=false 时 IP checksum 正确

**验收标准**：
- `xmake build test_net_header && xmake run test_net_header` 全部通过
- 新测试覆盖 fill/build 的正常路径和边界条件

**预估**：~200 行实现 + ~200 行测试

---

### 阶段 2: udp.hpp — UdpSender + build_udp_packet

**交付物**：
- `include/eph/dpdk/udp.hpp` 新文件
- `dpdk.hpp` 新增 `#include "eph/dpdk/udp.hpp"`
- `tests/test_udp.cpp` 新文件

**具体实现**：

1. **`UdpConfig::validate()`**：
   - 检查 src_ip, dst_ip, src_port, dst_port 非零
   - 检查 pool != nullptr
   - 返回 `std::string_view`（空=有效）

2. **`UdpSender::create(cfg)`**：
   - 调用 `cfg.validate()`，失败返回 error
   - 如果 `cfg.hw_cksum`：检查 NIC 支持 `RTE_ETH_TX_OFFLOAD_UDP_CKSUM`
     （通过 `rte_eth_dev_info_get` + 检查 `tx_offload_capa`）
   - 构造 `UdpPacketTemplate`，调用 `init()`
   - 存储 pool_, port_id_, tx_queue_id_
   - 日志 INFO: "UdpSender created: {src}:{port} -> {dst}:{port}, hw_cksum={}"

3. **`UdpSender::send(data, len)`**：
   - `tmpl_.build(pool_, data, len)` 获取 mbuf
   - 如果 nullptr：stats_.tx_errors++，return false
   - `rte_eth_tx_burst(port_id_, tx_queue_id_, &mbuf, 1)`
   - 如果返回 0：`rte_pktmbuf_free(mbuf)`，stats_.tx_errors++，return false
   - stats_.tx_packets++, stats_.tx_bytes += len
   - return true

4. **`UdpSender::send_batch(segs, count)`**：
   - cap count to 32（与 TCP burst 一致）
   - 循环 build count 个 mbuf
   - `rte_eth_tx_burst(port_id_, tx_queue_id_, mbufs, built)`
   - free 未发送的 mbuf
   - 更新 stats
   - return sent count

5. **`build_udp_packet()`**：
   - 创建临时 `UdpPacketTemplate`，`init()` + `build()`
   - 一站式便捷函数

**测试**（`test_udp.cpp`）：
- `UdpConfig_ValidateSuccess` — 有效配置返回空
- `UdpConfig_ValidateZeroIp` — src_ip=0 返回错误
- `UdpConfig_ValidateNullPool` — pool=null 返回错误
- `UdpSender_CreateSuccess` — 有效配置创建成功
- `UdpSender_CreateInvalidConfig` — 无效配置返回 error
- `UdpSender_SendSuccess` — send 返回 true（需 EAL + vdev）
- `UdpSender_SendBatchPartial` — batch 部分发送
- `UdpSender_StatsAccumulate` — 多次 send 后 stats 正确
- `BuildUdpPacket_CorrectHeaders` — 便捷函数构建的报文正确
- `BuildUdpPacket_NullPool` — 返回 nullptr

**验收标准**：
- `xmake build test_udp && xmake run test_udp` 全部通过
- `xmake build` 整体编译无 warning

**预估**：~250 行实现 + ~200 行测试

---

### 阶段 3: flow_steering.hpp — UDP Flow Rule 支持

**交付物**：
- `flow_steering.hpp` 新增 `FlowProtocol` 枚举
- `install_flow_rule` 新增 `proto` 参数（默认 `Tcp`，向后兼容）
- `test_flow_steering.cpp` 扩展

**具体实现**：

1. **`FlowProtocol` 枚举 + `flow_protocol_name()`**：放在 `RxDispatchMode` 之后

2. **`install_flow_rule` 修改**：
   - 新增参数 `FlowProtocol proto = FlowProtocol::Tcp`
   - 根据 proto 选择 pattern 中的 L4 层：
     ```cpp
     if (proto == FlowProtocol::Tcp) {
         // 现有 TCP pattern（rte_flow_item_tcp）
     } else {
         // UDP pattern（rte_flow_item_udp）
         rte_flow_item_udp udp_spec{};
         udp_spec.hdr.src_port = rte_cpu_to_be_16(tuple.dst_port);
         udp_spec.hdr.dst_port = rte_cpu_to_be_16(tuple.src_port);
         // ...
     }
     ```
   - 日志中标注协议类型

3. **`FlowRule::dump()` / `to_json()`**：不改（不持有协议信息）

4. **`std::formatter<FlowProtocol>` 特化**

**测试**（`test_flow_steering.cpp` 扩展）：
- `FlowProtocol_NameTcp` — 名称正确
- `FlowProtocol_NameUdp` — 名称正确
- `FlowProtocol_EnumValues` — 值不同
- `FlowProtocol_Format` — std::format 输出正确
- 现有 `install_flow_rule` 测试不受影响（默认参数 = Tcp）

**验收标准**：
- 现有 `test_flow_steering` 无回归
- 新测试通过
- `xmake build` 无 warning

**预估**：~60 行实现 + ~30 行测试

---

## 关键决策记录

### D-1: 两层抽象 vs 单层
- **问题**：UDP TX 用一个类还是两层
- **选项**：A. 单类 UdpSender 内含模板逻辑 / B. UdpPacketTemplate + UdpSender 分离
- **决策**：B
- **理由**：与 TCP 的 PacketTemplate + TcpSession 保持架构一致；高级用户可直接使用 UdpPacketTemplate 操作 mbuf
- **验收标准**：UdpSender 内部使用 UdpPacketTemplate，用户可通过 `packet_template()` 访问

### D-2: hw_cksum 默认值
- **问题**：UDP checksum offload 是否默认开启
- **决策**：默认 false（checksum=0）
- **理由**：IPv4 下 UDP checksum 可选（RFC 768），数据中心内网 L2 CRC 已提供完整性，checksum=0 是最低延迟选择
- **验收标准**：hw_cksum=false 时 UDP checksum 字段为 0，IP checksum 软件计算正确

### D-3: Flow Steering 向后兼容
- **问题**：install_flow_rule 签名变更如何兼容现有代码
- **决策**：新增参数使用默认值 `FlowProtocol::Tcp`
- **理由**：所有现有调用点不需要修改
- **验收标准**：现有 test_flow_steering 和 test_connector 无需修改即通过

### D-4: DNS 重构推迟
- **问题**：是否在本 PR 中让 dns.hpp 使用 UdpPacketTemplate
- **决策**：推迟到独立 PR
- **理由**：DNS 有 70+ 测试，混合改动增加回归风险
- **验收标准**：dns.hpp 在本 PR 中不被修改

### D-5: RX 路径预留
- **问题**：UdpConfig 是否包含 rx_queue_id
- **决策**：包含，类型 `std::optional<uint16_t>`，默认 nullopt
- **理由**：为未来 UDP RX 扩展预留配置位，不增加运行时开销
- **验收标准**：UdpSender::create 忽略 rx_queue_id（日志 DEBUG 提示 "rx_queue_id reserved for future use"）
