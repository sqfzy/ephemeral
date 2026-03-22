# eph-net 代码摘要

## 1. 概述

eph-net 是 ephemeral 项目中的网络传输层模块，提供低延迟的 WebSocket 客户端实现。
整个模块采用 C++23 header-only 风格编写（约 5400 行），基于模板和 concept 实现
零开销的多后端抽象——同一套 `Transport<TcpImpl>` 模板既可以使用 POSIX socket
后端，也可以使用 DPDK 内核旁路后端，编译时完全单态化。

核心设计思路是**控制面与数据面分离**：连接建立阶段（TCP → TLS 1.3 握手 →
WebSocket Upgrade）在主线程同步完成；数据传输阶段由独立的 TX/RX 线程通过
SPSC 无锁队列与应用线程通信，热路径上使用直接 AEAD 加解密而非 SSL_read/write，
消除 OpenSSL API 调用开销。

模块依赖 aws-lc（BoringSSL 兼容）进行 TLS 1.3 握手和 AES-256-GCM 加解密，
使用 spdlog 进行分级日志输出。通过 xmake 构建，作为 headeronly target 对外
暴露。

## 2. 架构

```
+---------------------------------------------------------------+
|                    Application Thread                         |
|   send() / recv()          stats()            stop()          |
+------+------------------+-------------------+-----------------+
       |                  |                   |
       v                  v                   v
  +---------+       +-----------+       +-----------+
  | TX SPSC |       | RX SPSC   |       | atomic    |
  | Queue   |       | Queue     |       | running_  |
  +---------+       +-----------+       +-----------+
       |                  ^                   |
       v                  |                   v
+------+-------+   +------+--------+   +-----+------+
|  TX Thread   |   |  RX Thread    |   | stop()     |
|  drain queue |   |  poll TCP rx  |   | join threads|
|  WS frame    |   |  TLS decrypt  |   | send Close |
|  TLS encrypt |   |  WS decode    |   | TCP close  |
|  TCP send    |   |  enqueue RX   |   +-----------+
+--------------+   +---------------+
       |                  ^
       v                  |
+------+------------------+-----+
|        TcpTransport           |  <-- concept 约束
|  (SocketTransport / DPDK)     |
+-------------------------------+
       |         ^
       v         |
   [ Network ]
```

## 3. 模块映射

| 模块/文件 | 职责 | 核心类型 | 依赖 |
|-----------|------|----------|------|
| `net.hpp` | 便捷总头文件 | — | tcp_concept, socket_transport |
| `tcp_concept.hpp` | TCP 后端抽象接口 | `TcpTransport` concept, `TcpState` enum | 标准库 |
| `socket_transport.hpp` | POSIX socket TCP 实现 | `SocketTransport`, `SocketConfig` | tcp_concept, spdlog |
| `tls_session.hpp` | TLS 1.3 握手 + 密钥导出 | `TlsSession<TcpImpl>`, `TlsHotState`, `TlsKeyMaterial` | tcp_concept, aws-lc (OpenSSL), spdlog |
| `tls_record.hpp` | TLS 记录层 AEAD 加解密 | `TlsRecordCrypto` | tls_session, aws-lc (EVP_AEAD), spdlog |
| `websocket.hpp` | WebSocket 协议 RFC 6455 | `DecodedFrame`, `FrameTemplate`, `MaskKeyCache` | aws-lc (RAND), spdlog |
| `http.hpp` | HTTP Upgrade 请求/响应 | `UpgradeResponse` | aws-lc (EVP/RAND), spdlog |
| `transport_types.hpp` | 公共类型定义 | `TransportConfig`, `TransportStats`, `RttStats`, `SendError` | 标准库 |
| `transport.hpp` | 传输主引擎 | `Transport<TcpImpl, MaxPayload, QueueDepth>` | 以上所有, eph-utils, eph-containers |

## 4. 数据流

### 发送路径 (热路径)

```
app send(data, len)
  |
  v
[UTF-8 校验 (文本帧)]
  |
  v
[SPSC TX Queue: try_produce → TxMessage]
  |
  v  (TX Thread)
[try_consume_n 批量取出]
  |
  v
[ws::encode_frame: 帧头 + masked_copy]
  |                     use_tls?
  +--- true ----------->  [TlsRecordCrypto::encrypt]
  |                           |
  +--- false --+              v
               |         [TCP send (TLS record)]
               |
               +-------> [TCP send (raw WS frame)]
```

### 接收路径 (热路径)

```
  [TCP poll_rx / recv()]
  |                     use_tls?
  +--- true ----------->  [TLS record 重组]
  |                           |
  +--- false --+              v
               |         [TlsRecordCrypto::decrypt]
               |              |
               +<-------------+
               |
               v
  [ws::decode_frame]
  |
  +-- Ping --> [enqueue Pong 到 TX Queue]
  +-- Pong --> [RTT 统计记录]
  +-- Close -> [设 closing_, 回复 Close]
  +-- Data --> [on_message 回调 或 SPSC RX Queue]
               |
               v
          app recv() / on_message()
```

### 连接建立流程

```
Transport::create(tcp_factory, config)
  |
  v
tcp_factory() → SocketTransport::connect()
  |  DNS 解析 (async + timeout)
  |  non-blocking connect + poll()
  v
[TLS 1.3 Handshake] (如果 use_tls=true)
  |  TlsSession::create() + handshake()
  |  自定义 BIO 桥接 TcpTransport
  v
[WebSocket Upgrade]
  |  http::build_upgrade_request()
  |  通过 TLS/TCP 发送 HTTP Upgrade
  |  解析 101 Switching Protocols
  v
[TLS 密钥导出]
  |  extract_hot_state() → TlsHotState
  |  TlsRecordCrypto::create()
  v
[启动 TX/RX 线程]
```

## 5. 核心组件

### 5.1 Transport (transport.hpp)

**文件**: `include/eph/net/transport.hpp` (2087 行)

主引擎类，模板参数化 TCP 后端、最大载荷大小和队列深度。

```cpp
template <TcpTransport TcpImpl,
          size_t MaxPayload = 512,
          size_t QueueDepth = 1024>
class Transport;
```

**公共接口**:
- `create(TcpFactory, TransportConfig)` — 工厂方法，完成全部握手后返回 `unique_ptr`
- `send() / send_text() / send_binary()` — 非阻塞发送，入队到 SPSC TX
- `send_for() / send_text_for()` — 带超时的发送（自旋等待队列空间）
- `send_n()` — 批量发送（单次原子尾指针更新）
- `send_close() / send_ping()` — WebSocket 控制帧
- `recv() / recv_n() / drain_recv()` — 非阻塞接收
- `wait_recv() / wait_recv_msg()` — 阻塞接收（带超时）
- `try_recv() / try_recv_msg()` — 返回拷贝的消息
- `close_gracefully()` — RFC 6455 优雅关闭
- `stop()` — 停止传输（join 线程 → 发送 Close → 关闭 TCP）
- `stats() / rtt_stats()` — 运行时统计
- `state() / is_connected() / is_running()` — 状态查询

**内部线程模型**:
- TX 线程: busy-poll SPSC 队列，批量 drain，WS 帧编码 + TLS 加密 + TCP 发送；
  周期性发送 Ping 帧并检测 Pong 超时
- RX 线程: busy-poll TCP rx，TLS 记录重组 + 解密，WS 帧解码，
  控制帧自动处理（Pong 回复、Close 响应），数据帧推送到 RX 队列或
  `on_message` 回调；断连时执行指数退避重连

**注意**: Transport 不可移动、不可复制，因为它拥有运行中的线程。

### 5.2 TcpTransport concept (tcp_concept.hpp)

**文件**: `include/eph/net/tcp_concept.hpp` (93 行)

C++20 concept，定义 TCP 后端必须满足的接口：

```cpp
template <typename T>
concept TcpTransport = requires(T& t, ...) {
    { t.connect(timeout) } -> std::same_as<std::expected<void, std::string>>;
    { t.send(data, len) }  -> std::same_as<std::expected<size_t, std::string>>;
    { t.poll_rx(callback) } -> std::same_as<std::expected<uint16_t, std::string>>;
    { t.close() }          -> std::same_as<std::expected<void, std::string>>;
    { t.reset() }          noexcept;
    { t.mss() }            -> std::convertible_to<uint16_t>;
    { t.state() }          -> std::same_as<TcpState>;
    { t.is_established() } -> std::same_as<bool>;
};
```

附带 `TcpState` 枚举（Closed, SynSent, Established, FinWait1/2, TimeWait, CloseWait, LastAck）。

### 5.3 SocketTransport (socket_transport.hpp)

**文件**: `include/eph/net/socket_transport.hpp` (769 行)

POSIX socket 实现，满足 `TcpTransport` concept（文件末尾有 `static_assert` 验证）。

关键特性:
- DNS 解析带超时保护（`std::async` + `wait_for`，避免 `getaddrinfo` 无限阻塞）
- 非阻塞 connect + `poll()` 等待
- TCP_NODELAY、SO_KEEPALIVE、自定义缓冲区大小
- MSS 查询（`TCP_MAXSEG`）
- 连接延迟统计（dns_latency_ns, connect_latency_ns）
- `poll_rx_for()` — 带超时的 poll 接收

文件末尾定义了便捷类型别名和工厂函数:
- `SocketWssTransport` = `Transport<SocketTransport, 512, 1024>`
- `SocketWssLargeTransport` = `Transport<SocketTransport, 4096, 512>`
- `socket_wss_connect()` — 一行创建 WSS 连接
- `socket_ws_connect()` — 一行创建 plain WS 连接

### 5.4 TlsSession (tls_session.hpp)

**文件**: `include/eph/net/tls_session.hpp` (672 行)

TLS 1.3 会话管理，模板参数化于 TCP 后端。

核心设计：使用**自定义 BIO** 桥接 OpenSSL 与 `TcpTransport`，BIO 的 read/write
回调直接调用 TCP 后端的 `send()/poll_rx()`。握手完成后导出 `TlsHotState`
（两个 cache-line 对齐的 `TlsKeyMaterial`，分别用于 TX/RX 方向），
数据面不再使用 SSL_write/SSL_read。

关键类型:
- `TlsKeyMaterial` — 64 字节（1 cache line），包含 AES key (32B) + IV (12B) + seq (8B)
- `TlsHotState` — 128 字节（2 cache lines），write + read 方向各一套
- `TlsConfig` — hostname (SNI), ca_cert_path, verify_peer, handshake_timeout
- `tls_keygen::hkdf_expand_label()` — RFC 8446 §7.1 HKDF-Expand-Label 实现

### 5.5 TlsRecordCrypto (tls_record.hpp)

**文件**: `include/eph/net/tls_record.hpp` (357 行)

TLS 记录层 AES-256-GCM 加解密，使用 aws-lc 的 `EVP_AEAD_CTX_seal/open` 单调用 API
（比多步 EVP_Encrypt Init/Update/Final 快约 150ns）。

```cpp
class TlsRecordCrypto {
    uint16_t encrypt(uint8_t* plaintext, uint16_t len, uint8_t* out);
    bool decrypt(const uint8_t* record, uint16_t len, uint8_t* out, uint16_t& out_len);
    static constexpr uint16_t encrypted_size(uint16_t plaintext_len);
};
```

**线程安全**: encrypt() 和 decrypt() 使用独立的 AEAD 上下文，TX 线程调用 encrypt()，
RX 线程调用 decrypt()，无竞争。

辅助函数:
- `tls_record::build_nonce()` — 优化的 nonce 构建（uint64_t XOR 替代逐字节循环）
- `tls_record::write_record_header()` / `parse_record_header()`

### 5.6 WebSocket (websocket.hpp)

**文件**: `include/eph/net/websocket.hpp` (572 行)

RFC 6455 WebSocket 协议实现，包含帧编解码、Masking、控制帧构建。

关键组件:
- `MaskKeyCache` — CSPRNG 批量预生成 1024 个 mask key（~2ns/key vs ~1500ns 逐帧 RAND_bytes）
- `masked_copy()` — 融合 memcpy + XOR masking 的单遍操作（64-bit 块处理）
- `encode_frame() / decode_frame()` — 完整帧编解码
- `FrameTemplate` — 预计算帧模板，热路径复用
- `is_valid_utf8()` — Bjoern Hoehrmann DFA 算法的单遍 UTF-8 校验
- `build_close_frame() / build_pong_frame() / build_ping_frame()` — 控制帧工厂

### 5.7 HTTP (http.hpp)

**文件**: `include/eph/net/http.hpp` (273 行)

最小化 HTTP/1.1 实现，仅支持 WebSocket Upgrade：

- `generate_ws_key()` — CSPRNG 生成 16 字节 Base64 编码的 WS key
- `build_upgrade_request()` — 构建 HTTP Upgrade 请求
- `parse_upgrade_response()` — 解析 101 Switching Protocols 响应
- `UpgradeResponse` — 解析结果（status_code, sec_ws_accept, sec_ws_protocol）

### 5.8 TransportConfig / TransportStats (transport_types.hpp)

**文件**: `include/eph/net/transport_types.hpp` (573 行)

公共类型，无重量级依赖（不引入 TLS/WS 头文件）：

- `SendError` — 类型安全的发送结果枚举
- `TransportEvent` / `TransportState` — 连接生命周期事件与状态
- `TransportConfig` — 完整连接配置（目标、TLS、超时、性能、重连、回调）
  含 `validate()` 编译期校验和 `dump()` / `to_json()` 序列化
- `TransportStats` — 聚合运行统计（TX/RX 包数、字节、丢弃、加解密错误、
  重连次数等），含速率辅助方法和 `to_json()`
- `RttStats` — RTT 百分位统计（p50, p99, p999）
- `ThreadStats` — 每线程计数器（避免原子操作竞争）

所有枚举类型均有 `std::formatter` 特化。

## 6. 入口点与 API

| 入口 | 类型 | 说明 |
|------|------|------|
| `socket_wss_connect<MaxPayload, QueueDepth>(config)` | 工厂函数 | 一行创建 WSS (TLS) 连接 |
| `socket_ws_connect<MaxPayload, QueueDepth>(config)` | 工厂函数 | 一行创建 plain WS 连接 |
| `Transport<TcpImpl>::create(factory, config)` | 静态工厂 | 通用创建（支持自定义 TCP 后端）|
| `Transport::send() / recv()` | 成员方法 | 非阻塞数据传输 |
| `Transport::stop() / close_gracefully()` | 成员方法 | 生命周期管理 |
| `Transport::stats() / rtt_stats()` | 成员方法 | 运行时监控 |
| `SocketWssTransport` | 类型别名 | `Transport<SocketTransport, 512, 1024>` |
| `SocketWssLargeTransport` | 类型别名 | `Transport<SocketTransport, 4096, 512>` |
| `SocketWsTransport` | 类型别名 | `Transport<SocketTransport, 512, 1024>` (plain WS) |

## 7. 依赖关系

### 内部模块依赖图

```
transport.hpp
  |
  +---> tcp_concept.hpp
  +---> transport_types.hpp ---> tcp_concept.hpp
  +---> tls_session.hpp -------> tcp_concept.hpp
  +---> tls_record.hpp --------> tls_session.hpp
  +---> websocket.hpp
  +---> http.hpp
  +---> eph-utils  (alignment, cpu, record)
  +---> eph-containers (bounded_queue)

socket_transport.hpp
  |
  +---> tcp_concept.hpp
  +---> transport.hpp (尾部 include, 定义类型别名)

net.hpp (便捷头文件)
  |
  +---> tcp_concept.hpp
  +---> socket_transport.hpp
```

### 外部依赖

| 包名 | 用途 | 引入位置 |
|------|------|----------|
| aws-lc | TLS 1.3 握手 (SSL API), AEAD 加解密 (EVP_AEAD), HKDF 密钥派生, CSPRNG (RAND_bytes) | tls_session, tls_record, websocket, http |
| spdlog | 分级日志 (SPDLOG_ACTIVE_LEVEL 编译期过滤) | 所有模块 |
| eph-utils | cache-line 对齐 (alignment), CPU 亲和 (cpu), HdrHistogram (record) | transport |
| eph-containers | SPSC 无锁队列 (BoundedQueue) | transport |

## 8. 测试

| 测试文件 | 行数 | 测试目标 | 关键场景 |
|----------|------|----------|----------|
| `test_tcp_concept.cpp` | 458 | TcpTransport concept | MockTcp 满足 concept、TcpState 枚举完整性 |
| `test_socket_transport.cpp` | 267 | SocketTransport | 配置校验、连接超时、DNS 失败 |
| `test_tls_record.cpp` | 624 | TlsRecordCrypto | 加解密往返、序列号递增、错误处理、nonce 构建 |
| `test_http.cpp` | 451 | HTTP Upgrade | 请求构建、101 响应解析、子协议、错误状态码 |
| `test_websocket.cpp` | 891 | WebSocket 协议 | 帧编解码、masking、控制帧、UTF-8 校验、碎片化 |
| `test_transport_types.cpp` | 612 | 公共类型 | Config 校验、Stats 格式化/JSON、SendError 枚举 |

**总测试代码**: 3303 行，覆盖 6 个模块。

**未覆盖**: `Transport` 类本身没有独立单元测试（需要真实网络连接），
集成测试通过 `ws_echo_client` 示例程序进行。`TlsSession` 的握手流程
同样依赖真实 TLS 服务器，无 mock 测试。
