# eph-dpdk 项目摘要

## 1. 概述

eph-dpdk 是 [ephemeral](https://github.com/user/ephemeral) 项目的 DPDK 后端子模块，提供基于 DPDK 的用户态 TCP 网络栈实现。它是一个纯头文件 (header-only) 的 C++23 库，绕过内核网络栈，通过 DPDK PMD (Poll Mode Driver) 直接与网卡通信，实现超低延迟的网络传输。

该库采用三层架构设计：底层 DPDK 平台初始化 (EAL/Port/Mempool)、中间层网络协议头构建与解析 (Ethernet/IPv4/TCP)、以及用户态 TCP 会话状态机。eph-dpdk 本身只负责 DPDK 特有的数据平面逻辑；上层的 TLS 1.3、WebSocket 帧编解码、HTTP Upgrade 等协议由兄弟模块 `eph-net` 以泛型方式提供，通过 C++20 concept (`TcpTransport`) 与 eph-dpdk 的 `TcpSession` 对接，实现零开销的编译期多态。

TCP 实现采用极简设计——仅实现 seq/ack 跟踪、窗口管理和 FIN/RST 处理，不实现重传、Nagle、拥塞控制等复杂机制。丢包策略为：检测到乱序/丢失后立即重连 (~2ms)，适用于数据中心内部几乎零丢包的场景（如交易所行情接入）。

## 2. 架构

```
+----------------------------------------------------------+
|                    应用层 (用户代码)                       |
|  DpdkTransport = Transport<TcpSession, 512, 1024>        |
+----------------------------------------------------------+
         |                           ^
         | send()/send_text()        | recv(callback)
         v                           |
+----------------------------------------------------------+
|              eph-net (泛型协议栈，独立模块)                |
|  transport.hpp ── SPSC 队列, TX/RX 线程, 自动重连          |
|  tls_session.hpp / tls_record.hpp ── TLS 1.3 握手/AEAD   |
|  websocket.hpp ── RFC 6455 帧编解码                       |
|  http.hpp ── HTTP/1.1 WebSocket Upgrade                  |
+----------------------------------------------------------+
         |                           ^
         | TcpTransport concept      | poll_rx(callback)
         v                           |
+----------------------------------------------------------+
|              eph-dpdk (本模块，DPDK 后端)                  |
|                                                          |
|  Layer 2: tcp.hpp ── 用户态 TCP 状态机                    |
|    - 三次握手 / 数据传输 / FIN-RST / 乱序缓冲            |
|    - net_header.hpp ── Eth/IPv4/TCP 头构建与解析          |
|    - arp.hpp ── 无状态 ARP 解析                           |
|                                                          |
|  Layer 1: platform.hpp ── 端口/队列/Mempool 初始化        |
|           eal.hpp ── EAL 生命周期管理                     |
+----------------------------------------------------------+
         |                           ^
         | rte_eth_tx_burst()        | rte_eth_rx_burst()
         v                           |
+----------------------------------------------------------+
|              DPDK PMD (af_packet / mlx5 / net_pcap)      |
|                        网卡硬件                           |
+----------------------------------------------------------+
```

## 3. 模块映射

| 模块/文件 | 职责 | 关键类型/函数 | 依赖 |
|-----------|------|---------------|------|
| `eal.hpp` | EAL 生命周期 (每进程一次) | `eal_init()`, `eal_cleanup()` | DPDK `rte_eal`, spdlog |
| `platform.hpp` | NIC 端口/队列/Mempool 初始化 | `Platform`, `PlatformConfig`, `validate_config()` | DPDK `rte_ethdev/rte_mempool`, spdlog |
| `net_header.hpp` | Eth/IPv4/TCP 头构建、校验和、包解析 | `PacketTemplate`, `ParsedPacket`, `ConnectionTuple`, `parse_packet()` | DPDK `rte_mbuf/rte_ether/rte_ip/rte_tcp` |
| `arp.hpp` | 无状态 ARP 请求/应答解析 | `ArpPacket`, `resolve()`, `build_arp_request()` | `net_header.hpp`, DPDK, spdlog |
| `tcp.hpp` | 用户态 TCP 状态机 | `TcpSession`, `TcpConfig`, `TcpSession::Stats` | `net_header.hpp`, `eph::net::TcpTransport` concept, OpenSSL/aws-lc (`RAND_bytes`), spdlog |
| `types.hpp` | DPDK Transport 类型别名 | `DpdkTransport`, `DpdkSmallTransport`, `DpdkLargeTransport` | `tcp.hpp`, `eph::net::Transport` |

## 4. 数据流

### 发送路径 (TX)

```
应用调用 send_text(data, len)
    |
    v
eph-net Transport: WS 帧编码 -> TLS AEAD 加密
    |
    v
TcpSession::send(encrypted_data, len)
    |
    v
PacketTemplate::build_packet(pool, seq, ack, flags, ...)
    |  构建 Ethernet + IPv4 + TCP 头 + payload
    |  计算 IP/TCP 校验和 (软件或硬件卸载)
    v
rte_eth_tx_burst(port_id, queue_id, &mbuf, 1)
    |
    v
NIC 发送
```

### 接收路径 (RX)

```
NIC 收包 -> rte_eth_rx_burst(port_id, queue_id, pkts, 32)
    |
    v
TcpSession::poll_rx(callback) / process_rx(pkts, nb)
    |  parse_packet(): 零拷贝解析 Ethernet/IPv4/TCP 头
    |  匹配 ConnectionTuple (src/dst IP:port)
    |  处理 RST/FIN/ACK 控制包
    |  乱序检测 -> 缓冲 (ReorderEntry[8]) 或丢包重连
    |  顺序数据 -> 回调 data_callback(payload, len)
    v
eph-net Transport: TLS AEAD 解密 -> WS 帧解码
    |
    v
应用 recv(callback) 获取明文数据
```

### 连接建立流程

```
eal_init() -> Platform::create() -> ARP resolve()
    -> TcpSession::connect() [三次握手]
    -> TlsSession [TLS 1.3 握手, aws-lc custom BIO]
    -> HTTP Upgrade [WebSocket]
    -> 数据传输就绪
```

## 5. 关键组件

### 5.1 TcpSession (`include/eph/dpdk/tcp.hpp`)

用户态 TCP 状态机，满足 `eph::net::TcpTransport` concept。

- **状态机**: Closed -> SynSent -> Established -> FinWait1/2 -> TimeWait/Closed
- **ISN 生成**: 使用 `RAND_bytes()` (CSPRNG, RFC 6528)
- **乱序处理**: 8 槽 `ReorderEntry` 缓冲区，每槽最多 1460 字节；缓冲区满则判定为真实丢包，返回错误触发上层重连
- **关键接口**: `connect()`, `send()`, `poll_rx()`, `process_rx()`, `close()`, `reset()`, `build_data_packet()` (热路径零分配)
- **模板约束**: `static_assert(eph::net::TcpTransport<TcpSession>)` 编译期验证

```cpp
class TcpSession {
    std::expected<void, std::string> connect(milliseconds timeout);
    std::expected<size_t, std::string> send(const void* data, size_t len);
    template <typename F> std::expected<uint16_t, std::string> poll_rx(F&&);
    rte_mbuf* build_data_packet(rte_mbuf*, const void*, uint16_t); // 热路径
};
```

### 5.2 PacketTemplate (`include/eph/dpdk/net_header.hpp`)

预填充静态字段的 Ethernet/IPv4/TCP 头模板，热路径仅更新动态字段 (seq, ack, flags, payload)。

- **`build_packet()`**: 从 mempool 分配 mbuf 并构建完整包，支持 SYN 选项 (MSS/SACK/WScale)
- **`fill_packet()`**: 在已有 mbuf 上原地构建包 (零分配热路径)
- **校验和**: 支持软件计算 (`internet_checksum()` / `tcp_checksum()`) 和 NIC 硬件卸载 (`hw_cksum` 标志)

### 5.3 ParsedPacket / parse_packet() (`include/eph/dpdk/net_header.hpp`)

零拷贝包解析器。指针直接指向 mbuf 数据区域，不复制任何数据。

- 校验 EtherType、IP IHL、TCP data_off
- 使用 IP `total_length` 计算 payload 长度 (避免 Ethernet 填充字节污染)
- `matches(ConnectionTuple)` 匹配连接四元组 (自动交换 src/dst)

### 5.4 Platform (`include/eph/dpdk/platform.hpp`)

DPDK NIC 端口初始化封装。

- **初始化链**: enumerate_ports -> create_mempool -> configure_port -> setup_queues -> start_port -> wait_link_up
- **PlatformConfig 验证**: `validate_config()` 是 `constexpr` 函数，支持 `static_assert` 编译期验证
- **描述符对齐**: `clamp_desc()` 将请求的描述符数量对齐到 NIC 硬件限制 (constexpr)
- **Mempool 约束**: pool size 必须为 2^n - 1，由 `is_power_of_two_minus_one()` 验证

### 5.5 EAL 管理 (`include/eph/dpdk/eal.hpp`)

极简的 EAL 生命周期包装。

- `eal_init()`: 包装 `rte_eal_init()`，返回 `std::expected<int, std::string>`
- `eal_cleanup()`: 包装 `rte_eal_cleanup()`
- 与 Platform 分离设计：EAL 是进程级单例，Platform 是端口级实例

### 5.6 ARP 解析 (`include/eph/dpdk/arp.hpp`)

无状态阻塞式 ARP 解析，用于连接建立前获取网关 MAC 地址。

- `resolve()`: 发送 ARP 广播请求，busy-poll 等待应答，最多重试 3 次
- 非热路径——仅在 TCP 连接建立前调用一次
- `ArpPacket`: 28 字节 packed 结构体，`static_assert(sizeof == 28)`

### 5.7 类型别名 (`include/eph/dpdk/types.hpp`)

将 DPDK 后端的 `TcpSession` 与 eph-net 泛型 Transport 组合：

```cpp
using DpdkTransport      = Transport<TcpSession, 512, 1024>;
using DpdkSmallTransport = Transport<TcpSession, 64, 256>;
using DpdkLargeTransport = Transport<TcpSession, 4096, 512>;
```

## 6. 入口点与 API

| 入口点 | 文件 | 说明 |
|--------|------|------|
| `eal_init(argc, argv)` | `eal.hpp` | 初始化 DPDK EAL (每进程一次) |
| `Platform::create(config)` | `platform.hpp` | 初始化 NIC 端口/队列/Mempool |
| `TcpSession(config, pool)` | `tcp.hpp` | 创建 TCP 会话 |
| `TcpSession::connect(timeout)` | `tcp.hpp` | 阻塞式三次握手 |
| `TcpSession::send(data, len)` | `tcp.hpp` | 发送数据 (单 MSS 限制) |
| `TcpSession::poll_rx(callback)` | `tcp.hpp` | 轮询收包并处理 |
| `TcpSession::close()` / `reset()` | `tcp.hpp` | 优雅关闭 / 强制 RST |
| `arp::resolve(...)` | `arp.hpp` | 阻塞式 ARP 地址解析 |
| `DpdkTransport::create(pool, config)` | `types.hpp` + `eph-net` | 完整 WSS 传输 (TCP+TLS+WS) |

## 7. 依赖关系

### 内部模块依赖图

```
types.hpp
    |
    +---> tcp.hpp
    |        |
    |        +---> net_header.hpp
    |        |        |
    |        |        +---> DPDK (rte_mbuf, rte_ether, rte_ip, rte_tcp)
    |        |
    |        +---> eph::net::TcpTransport (tcp_concept.hpp)
    |        +---> OpenSSL/aws-lc (RAND_bytes)
    |
    +---> eph::net::Transport (transport.hpp)
    +---> eph::net::TransportConfig (transport_types.hpp)

arp.hpp
    |
    +---> net_header.hpp

platform.hpp
    |
    +---> DPDK (rte_ethdev, rte_mempool, rte_lcore)

eal.hpp
    |
    +---> DPDK (rte_eal)
```

### 外部依赖

| 包 | 用途 | 必需? |
|----|------|-------|
| [DPDK](https://www.dpdk.org/) | NIC PMD, mbuf, EAL, ethdev | 是 |
| [aws-lc](https://github.com/aws/aws-lc) | CSPRNG (`RAND_bytes`), TLS 1.3 (via eph-net) | 是 (ISN 生成 + TLS) |
| [spdlog](https://github.com/gabime/spdlog) | 结构化日志，编译期级别过滤 (`SPDLOG_ACTIVE_LEVEL`) | 是 |
| [Google Test](https://github.com/google/googletest) | 单元测试 | 仅测试 |
| [Google Benchmark](https://github.com/google/benchmark) | 性能基准测试 | 仅基准 |
| eph-utils | 工具库 (通过 xmake 依赖) | 是 |
| eph-containers | 容器库 (SPSC 队列等) | 是 |
| eph-net | 泛型协议栈 (TLS/WS/HTTP/Transport) | 是 (types.hpp) |

## 8. 测试

| 测试文件 | 测试目标 | 需要 EAL? | 行数 |
|----------|----------|-----------|------|
| `tests/test_net_header.cpp` | 字节序、校验和、IPv4 解析/格式化、包构建/解析 | 否 | 489 |
| `tests/test_dpdk_platform.cpp` | PlatformConfig 验证、Platform 创建/销毁 | 是 (net_null) | 81 |
| `tests/dpdk_test_env.hpp` | GTest Environment，使用 `--no-huge --no-pci --vdev=net_null0` 启动 EAL | - | - |

### 关键测试场景

- **字节序**: `hton16/hton32` 往返一致性，constexpr 可用性
- **校验和**: `internet_checksum()` RFC 1071 合规，`tcp_checksum()` 含伪头计算
- **IPv4 解析**: 正常地址、边界值 (0.0.0.0, 255.255.255.255)、非法输入
- **包构建**: `PacketTemplate::build_packet()` SYN/ACK/PSH+ACK 完整性
- **包解析**: `parse_packet()` 各种畸形包拒绝 (过短、错误 EtherType、非 TCP)
- **Platform**: 非法 pool size 返回错误，port_id 越界返回错误

### 基准测试

| 基准文件 | 测试内容 |
|----------|----------|
| `benchmarks/bench_tcp_header.cpp` | TCP 头构建/解析性能 (163 行) |
| `benchmarks/bench_pipeline.cpp` | 端到端 TX/RX 管线延迟 (190 行) |
