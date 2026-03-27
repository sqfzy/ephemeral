# Project: ephemeral (eph)

> 面向超低延迟场景的 C++23 header-only 网络库，提供从 POSIX socket 到 DPDK 用户态网络栈的统一 WebSocket/TLS 传输抽象。

**Language**: C++23 | **Build**: xmake | **Version**: 1.0.0

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

---

## Overview

ephemeral 是一套为金融交易所行情接入等超低延迟场景设计的 C++23 网络库。项目采用完全 header-only 的设计，分为六个独立模块：`eph-utils`（底层工具）、`eph-containers`（无锁队列）、`eph-net`（基于 POSIX socket 的 WebSocket/TLS 传输层）、`eph-dpdk`（基于 DPDK 用户态网络栈的传输后端）、`eph-itch`（ITCH 协议解析）、`eph-fix`（FIX 协议解析）。

核心设计目标是通过 C++20 concepts（`TcpTransport`）实现传输后端的零开销抽象——同一套 `Transport<TcpImpl>` 模板可以在 POSIX socket 和 DPDK 用户态 TCP 之间无缝切换，上层协议栈（TLS 1.3 + WebSocket RFC 6455）完全复用。

在 DPDK 后端下，整个数据路径绕过内核网络栈：应用层 → SPSC 队列 → TX 线程（WS 帧编码 → TLS AES-GCM 加密 → TCP/IP 头构建）→ NIC PMD 直发。实测在 Intel Xeon (AES-NI) 上，64 字节载荷的端到端 TX 延迟约 164ns，RX 约 139ns。

TLS 数据面使用 aws-lc 的 `EVP_AEAD_CTX_seal/open` 单次调用 AEAD，绕过 `SSL_write/SSL_read` 的多层间接，节省约 150ns/record。WebSocket 掩码采用批量预生成 CSPRNG 密钥池（1024 键缓存），单键消耗降至约 2ns。

---

## Architecture

分层管道架构：四个模块自底向上组合，各层通过 C++ concepts 和模板参数化解耦。

### Component Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                      Application                            │
│              (ws_echo_client / user code)                    │
└─────────────┬───────────────────────────────┬───────────────┘
              │                               │
┌─────────────▼───────────────┐ ┌─────────────▼───────────────┐
│     eph-net (Socket路径)     │ │     eph-dpdk (DPDK路径)      │
│                             │ │                             │
│  Transport<SocketTransport> │ │  Transport<TcpSession>      │
│  ┌───────────────────────┐  │ │  ┌───────────────────────┐  │
│  │ WebSocket (RFC 6455)  │  │ │  │ WebSocket (RFC 6455)  │  │
│  │ TLS 1.3 (aws-lc)     │  │ │  │ TLS 1.3 (aws-lc)     │  │
│  │ HTTP Upgrade          │  │ │  │ HTTP Upgrade          │  │
│  └───────────┬───────────┘  │ │  └───────────┬───────────┘  │
│  ┌───────────▼───────────┐  │ │  ┌───────────▼───────────┐  │
│  │  SocketTransport      │  │ │  │  TcpSession (用户态)   │  │
│  │  (POSIX socket/poll)  │  │ │  │  (seq/ack, no retx)   │  │
│  └───────────┬───────────┘  │ │  └───────────┬───────────┘  │
│              │              │ │  ┌───────────▼───────────┐  │
│              │              │ │  │  Platform / EAL       │  │
│              │              │ │  │  (NIC PMD, mempool)   │  │
│              │              │ │  └───────────┬───────────┘  │
└──────────────┼──────────────┘ └──────────────┼──────────────┘
               │                               │
        ┌──────▼──────┐                 ┌──────▼──────┐
        │ Kernel TCP  │                 │   NIC HW    │
        │   Stack     │                 │ (PMD 直通)   │
        └─────────────┘                 └─────────────┘
```

### 统一接口层

```
┌──────────────────────────────────────────────┐
│       concept TcpTransport                   │
│  connect() | send() | poll_rx() | close()    │
│  reset()   | mss()  | state()               │
└─────────┬────────────────────┬───────────────┘
          │                    │
  SocketTransport        TcpSession (DPDK)
```

### 共享基础设施

```
┌─────────────────────────────────────────┐
│            eph-containers               │
│  BoundedQueue<T,N>  EvictingQueue<T,N>  │
│  BoundedQueueBytes  EvictingQueueBytes  │
└─────────────────┬───────────────────────┘
                  │
┌─────────────────▼───────────────────────┐
│              eph-utils                  │
│  TSC | HdrHistogram | Recorder | CPU    │
│  HugePage | Alignment | Version        │
└─────────────────────────────────────────┘
```

---

## Module Map

| Module / File | Responsibility | Key Types | Depends On |
|---|---|---|---|
| **eph-utils** | | | |
| `utils/time.hpp` | TSC 硬件计时器（rdtscp/cntvct_el0），纳秒级精度 | `TSC` | stdlib, spdlog |
| `utils/cpu.hpp` | CPU 拓扑探测、线程亲和性绑定、自旋暂停指令 | `CpuTopologyInfo` | stdlib, spdlog |
| `utils/alignment.hpp` | Cache line 对齐常量与模板 | `CACHE_LINE_SIZE`, `Align<T>` | stdlib |
| `utils/hugepage.hpp` | 大页内存分配（mmap → aligned_alloc 降级） | `HugePage` | stdlib, spdlog |
| `utils/record.hpp` | HDR 直方图、单线程/多线程性能记录器、系统资源统计 | `HdrHistogram`, `Recorder`, `ConcurrentRecorder`, `SystemStats` | `time.hpp`, stdlib |
| `version.hpp` | 编译期版本号 (1.0.0) | `kVersion*` | stdlib |
| **eph-containers** | | | |
| `containers/concepts.hpp` | `TrivialData` concept 约束 | `TrivialData<T>` | stdlib |
| `containers/bounded_queue.hpp` | SPSC 无锁有界队列，写满阻塞 | `BoundedQueue<T, Capacity>` | `concepts.hpp`, `alignment.hpp`, `cpu.hpp` |
| `containers/bounded_queue_bytes.hpp` | 变长字节载荷包装器（带时间戳） | `BoundedQueueBytes<MaxDataSize, Capacity>` | `bounded_queue.hpp` |
| `containers/evicting_queue.hpp` | SPSC 无等待写入队列，写满覆盖旧数据（seqlock） | `EvictingQueue<T, Capacity>` | `concepts.hpp`, `alignment.hpp`, `cpu.hpp` |
| `containers/evicting_queue_bytes.hpp` | 变长字节载荷包装器（带丢弃计数） | `EvictingQueueBytes<MaxDataSize, Capacity>` | `evicting_queue.hpp` |
| **eph-net** | | | |
| `net/tcp_concept.hpp` | TCP 传输后端 concept 定义 | `TcpState`, `TcpTransport` concept | stdlib |
| `net/transport_types.hpp` | 传输层公共类型（枚举、配置、统计） | `TransportConfig`, `ConnectionErrorInfo`, `SendError`, `TransportEvent` | stdlib |
| `net/socket_transport.hpp` | POSIX socket TCP 后端（非阻塞、poll I/O） | `SocketConfig`, `SocketTransport` | `tcp_concept.hpp`, spdlog, POSIX |
| `net/tls_session.hpp` | TLS 1.3 握手管理，自定义 BIO 桥接 TCP 后端 | `TlsSession<TcpImpl>`, `TlsHotState`, `TlsKeyMaterial` | `tcp_concept.hpp`, aws-lc, spdlog |
| `net/tls_record.hpp` | TLS record 层 AES-GCM AEAD 加解密 | `seal_record()`, `open_record()` | `tls_session.hpp`, aws-lc |
| `net/websocket.hpp` | RFC 6455 WebSocket 帧编解码、掩码、CSPRNG 密钥池 | `DecodedFrame`, `MaskKeyCache` | aws-lc (RAND), spdlog |
| `net/http.hpp` | 最小 HTTP/1.1（仅 WebSocket Upgrade 握手） | `UpgradeResponse` | aws-lc (EVP, RAND), spdlog |
| `net/transport.hpp` | 泛型 WebSocket 传输层（线程管理、SPSC 队列收发） | `Transport<TcpImpl, MaxPayload, QueueDepth>` | 所有 net/* 模块, eph-containers, eph-utils |
| **eph-dpdk** | | | |
| `dpdk/eal.hpp` | DPDK EAL 生命周期管理（进程级单例） | `EalGuard`, `eal_init()` | DPDK, spdlog |
| `dpdk/platform.hpp` | NIC 端口/队列/内存池初始化 | `Platform`, `PlatformConfig` | DPDK, spdlog |
| `dpdk/net_header.hpp` | Ethernet/IPv4/TCP 头构建与解析，constexpr 校验和 | `PacketTemplate`, `ParsedPacket`, `ConnectionTuple` | DPDK |
| `dpdk/tcp.hpp` | 用户态最小 TCP 状态机（三次握手、seq/ack、无重传） | `TcpSession`, `TcpConfig` | `net_header.hpp`, `tcp_concept.hpp`, DPDK |
| `dpdk/arp.hpp` | 无状态 ARP 解析（阻塞式广播请求/应答） | `ArpPacket`, `resolve()` | `net_header.hpp`, DPDK |
| `dpdk/connector.hpp` | 一站式连接助手（Platform→ARP→TCP→Transport） | `ConnectorConfig`, `ConnectResult`, `connect()` | 所有 dpdk/* 模块 |
| `dpdk/types.hpp` | DPDK 传输类型别名 | `DpdkTransport`, `DpdkSmallTransport`, `DpdkLargeTransport` | `tcp.hpp`, `net/transport.hpp` |

---

## Data Flow

### 发送路径 (TX)

应用调用 `Transport::send()` 将消息写入 TX SPSC 队列（`BoundedQueue`），TX 线程从队列取出消息后依次执行：WebSocket 帧编码（含掩码 XOR）→ TLS 1.3 AES-GCM 加密（`seal_record`）→ TCP 段发送。在 socket 路径下通过 `send()` 系统调用写入内核；在 DPDK 路径下构建 Ethernet/IP/TCP 头（`PacketTemplate::build_packet`）后通过 `rte_eth_tx_burst()` 直发 NIC。

### 接收路径 (RX)

RX 线程轮询 TCP 层（socket: `poll()` + `recv()`；DPDK: `rte_eth_rx_burst()` + `process_rx()`），收到数据后执行：TCP seq/ack 处理 → TLS record 重组与 AEAD 解密（`open_record`）→ WebSocket 帧解码 → 写入 RX SPSC 队列。应用通过 `Transport::recv(callback)` 非阻塞消费。

### Flow Diagram

```
  Application
      │
      │ send(data, len)
      ▼
 ┌──────────┐
 │ TX Queue │  BoundedQueue<TxMessage, QueueDepth>
 │  (SPSC)  │
 └────┬─────┘
      │ TX Thread
      ▼
 ┌──────────┐
 │ WS Encode│  websocket::encode_frame() + MaskKeyCache
 └────┬─────┘
      ▼
 ┌──────────┐
 │TLS Seal  │  tls_record::seal_record() (AES-256-GCM)
 └────┬─────┘
      ▼
 ┌──────────┐     ┌──────────────┐
 │TCP Send  │────►│ Kernel / NIC │
 └──────────┘     └──────┬───────┘
                         │
                         │ (wire)
                         │
                  ┌──────▼───────┐
                  │ Kernel / NIC │
 ┌──────────┐     └──────┬───────┘
 │TCP Poll  │◄───────────┘
 └────┬─────┘     RX Thread
      ▼
 ┌──────────┐
 │TLS Open  │  tls_record::open_record()
 └────┬─────┘
      ▼
 ┌──────────┐
 │WS Decode │  websocket::decode_frame()
 └────┬─────┘
      ▼
 ┌──────────┐
 │ RX Queue │  BoundedQueue<RxMessage, QueueDepth>
 │  (SPSC)  │
 └────┬─────┘
      │ recv(callback)
      ▼
  Application
```

---

## Key Components

### `Transport<TcpImpl, MaxPayload, QueueDepth>`

**File**: `eph-net/include/eph/net/transport.hpp`
**Purpose**: 泛型 WebSocket 传输层核心。管理 TX/RX 线程、SPSC 队列、连接生命周期（握手、重连、优雅关闭）。
**Interface**:
```cpp
static std::expected<std::unique_ptr<Transport>, ConnectionErrorInfo>
    create(TcpFactory factory, const TransportConfig& config);
SendError send(const void* data, size_t len, uint8_t opcode);
bool recv(auto&& callback);  // callback(const uint8_t* data, uint16_t len)
void close_gracefully(uint16_t code, std::string_view reason, duration timeout);
```
**Notes**: TcpImpl 可以是 `SocketTransport`（socket 路径）或 `TcpSession`（DPDK 路径），通过 concept 约束实现编译期多态。队列深度和最大载荷均为模板参数，编译期固定。

### `BoundedQueue<T, Capacity>`

**File**: `eph-containers/include/eph/containers/bounded_queue.hpp`
**Purpose**: SPSC 无锁有界队列，用于 TX/RX 线程与应用线程之间的数据传递。写满时阻塞（自旋等待）。
**Interface**:
```cpp
bool try_push(const T& item);
bool try_pop(T& item);
void produce(auto&& visitor);   // zero-copy write
void consume(auto&& visitor);   // zero-copy read
```
**Notes**: Cache line 对齐防止伪共享。影子索引（shadow head/tail）减少跨核原子操作。Capacity 必须是 2 的幂。

### `EvictingQueue<T, Capacity>`

**File**: `eph-containers/include/eph/containers/evicting_queue.hpp`
**Purpose**: SPSC 无等待写入队列。写满时覆盖最旧数据（不阻塞写者）。使用 seqlock 机制实现乐观读。
**Interface**:
```cpp
void push(const T& item);          // 永不阻塞
bool try_pop_latest(T& item);      // 读最新值
bool try_consume_latest(auto&& visitor);
```
**Notes**: 写者完全无等待（wait-free），读者通过 seqlock 进行乐观读（检测撕裂则重试）。适用于行情快照等"只关心最新值"的场景。

### `TcpSession` (DPDK)

**File**: `eph-dpdk/include/eph/dpdk/tcp.hpp`
**Purpose**: 用户态最小 TCP 实现。三次握手、seq/ack 跟踪、窗口管理、FIN/RST 处理。不实现重传/拥塞控制——丢包即重连（数据中心场景可接受）。
**Interface**:
```cpp
std::expected<void, std::string> connect(duration timeout);
std::expected<size_t, std::string> send(const void* data, size_t len);
std::expected<uint16_t, std::string> poll_rx(auto&& callback);
```
**Notes**: 满足 `TcpTransport` concept。乱序缓冲区（8 slot × 1460 byte）处理轻微乱序。ISN 使用 CSPRNG 生成。

### `SocketTransport`

**File**: `eph-net/include/eph/net/socket_transport.hpp`
**Purpose**: POSIX socket TCP 后端。非阻塞 socket + `poll()` I/O 复用。DNS 解析带超时保护（`std::async` + `wait_for`）。
**Interface**:
```cpp
std::expected<void, std::string> connect(duration timeout);
std::expected<size_t, std::string> send(const void* data, size_t len);
std::expected<uint16_t, std::string> poll_rx(auto&& callback);
```
**Notes**: 满足 `TcpTransport` concept。支持 TCP_NODELAY、SO_KEEPALIVE、缓冲区大小配置。JSON 序列化对 host 字段做注入防护。

### `TlsSession<TcpImpl>` + `tls_record`

**File**: `eph-net/include/eph/net/tls_session.hpp`, `tls_record.hpp`
**Purpose**: TLS 1.3 握手（通过 aws-lc 自定义 BIO 桥接任意 TCP 后端）+ 数据面 AEAD 加解密。握手后提取流量密钥，数据面直接调用 `EVP_AEAD_CTX_seal/open`，绕过 `SSL_write/SSL_read`。
**Interface**:
```cpp
// tls_session.hpp
std::expected<void, std::string> handshake();
TlsHotState& hot_state();  // 返回 key/iv/seq 用于数据面

// tls_record.hpp
std::expected<size_t, std::string> seal_record(out, key, iv, seq, plaintext, len);
std::expected<size_t, std::string> open_record(out, key, iv, seq, ciphertext, len);
```
**Notes**: `TlsHotState` 将读写密钥材料分布在两条 cache line 上，防止 TX/RX 线程伪共享。支持 AES-128-GCM 和 AES-256-GCM（根据协商密码动态选择）。

### `HdrHistogram` + `Recorder`

**File**: `eph-utils/include/eph/utils/record.hpp`
**Purpose**: 基于 Gil Tene 算法的高动态范围直方图，配合 TSC 硬件计时器实现纳秒级延迟分布记录。`ConcurrentRecorder` 使用 `thread_local` + `shared_ptr` 实现零竞争多线程记录。
**Interface**:
```cpp
bool record(uint64_t cycles);
std::optional<Stats> compute_stats();  // p50, p90, p99, p999
void print_report();
bool export_json(const std::string& dir);
```
**Notes**: 默认范围 1 cycle 到 10 秒，仅约 20KB 内存。线程退出时自动合并到退休缓冲区。

---

## Entry Points & APIs

| Entrypoint | Type | Description |
|---|---|---|
| `examples/ws_echo_client.cpp` | Binary | 统一 WebSocket echo 客户端，支持 socket/DPDK 双后端 |
| `Transport::create(factory, config)` | Library API | 创建 WebSocket 传输实例（泛型，后端由 TcpFactory 决定） |
| `eph::dpdk::connect<T>(cfg, tcfg)` | Library API | DPDK 一站式连接（Platform→ARP→TCP→Transport） |
| `eph::dpdk::eal_init(argc, argv)` | Library API | DPDK EAL 初始化（进程级单例） |
| `Platform::create(config)` | Library API | DPDK NIC 端口初始化 |

---

## Dependencies

### Internal (module graph)

```
eph-dpdk ──► eph-net (仅 tcp_concept.hpp, transport_types.hpp)
         ──► eph-containers ──► eph-utils
eph-net  ──► eph-containers ──► eph-utils
```

### External

| Package | Version | Purpose |
|---|---|---|
| `spdlog` | — | 结构化日志，支持编译期级别过滤 (SPDLOG_ACTIVE_LEVEL) |
| `aws-lc` | — | TLS 1.3 握手 (SSL)、AES-GCM AEAD (EVP_AEAD)、SHA-1 (EVP)、CSPRNG (RAND) |
| `DPDK` | — | NIC PMD、mbuf、EAL、ethdev（仅 eph-dpdk 模块） |
| `numactl` | — | NUMA 支持（可选） |
| `gtest` | — | 单元测试框架 |
| `benchmark` | — | Google Benchmark 微基准测试 |
| `tabulate` | — | 终端表格格式化（benchmark 输出） |

---

## Testing

| Test Suite | Location | Coverage Focus |
|---|---|---|
| Containers 单元测试 | `tests/containers/` | BoundedQueue、EvictingQueue 及其 Bytes 变体 |
| Utils 单元测试 | `tests/utils/` | TSC、CPU 拓扑、大页、对齐、Record、版本号 |
| Net 单元测试 | `tests/net/` | TLS record、HTTP upgrade、WebSocket 帧、TCP concept、socket transport、transport types |
| DPDK 单元测试 | `tests/dpdk/` | 网络头构建/解析、ARP、EAL、connector、platform |

Key test scenarios:
- BoundedQueue: 满队列/空队列边界、批量 push/pop、超时语义、零拷贝 produce/consume
- EvictingQueue: seqlock 撕裂检测、覆盖计数、单 slot 特化
- TLS record: nonce 构建正确性、加解密往返一致性、边界载荷大小
- WebSocket: 帧编解码往返、掩码 XOR 正确性、UTF-8 校验、控制帧长度限制、close code 合法性
- TCP concept: 静态 concept 满足性检查
- Socket transport: JSON 注入防护、配置校验与告警

### Benchmarks

| Benchmark Suite | Location | What It Measures |
|---|---|---|
| `bench_bq_pingpong` | `benchmarks/containers/` | SPSC 跨核往返延迟 |
| `bench_bq_throughput` | `benchmarks/containers/` | SPSC 峰值吞吐 |
| `bench_bq_pushpop` | `benchmarks/containers/` | 单线程 push/pop 开销 |
| `bench_bq_batch` | `benchmarks/containers/` | 批量操作性能 |
| `bench_eq_*` | `benchmarks/containers/` | EvictingQueue 同类基准 |
| `bench_ws` | `benchmarks/net/` | WebSocket 掩码/编码/解码微基准 |
| `bench_tls` | `benchmarks/net/` | TLS seal/open 微基准 |
| `bench_transport_pipeline` | `benchmarks/net/` | 端到端管道延迟（PlainWS / WSS / WSS Burst） |
| `bench_rx_pipeline` | `benchmarks/net/` | RX 管线微基准：decrypt→decode→filter（in-memory） |
| `bench_pipeline` | `benchmarks/dpdk/` | DPDK 管道基准 |
| `bench_tcp_header` | `benchmarks/dpdk/` | 网络头构建/校验和性能 |
| `bench_market` / `_dpdk` | `benchmarks/` | 单 symbol 行情延迟（Socket / DPDK） |
| `bench_market_multi` / `_dpdk` | `benchmarks/` | 多 symbol combined stream + twophase filter |
| `bench_market_persymbol_dpdk` | `benchmarks/` | Per-symbol 独立连接 + SharedRxDispatcher |
| `bench_pingpong` / `_dpdk` | `benchmarks/` | Ping-pong RTT（Socket / DPDK） |
| `bench_fix_parse` / `bench_itch_parse` | `benchmarks/` | FIX / ITCH 消息解析吞吐 |

### 工具

| 工具 | 位置 | 用途 |
|------|------|------|
| `mock_binance_server.py` | `tools/` | Mock Binance WebSocket server，可控 batch-size/rate/jitter |
