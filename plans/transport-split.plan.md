# Plan: TransportMode 拆分

> 将 Transport<TcpImpl, Framer, Mode, ...> 拆为 3 个独立类，使用组合模式替代 if constexpr 分支，提取 4 个可复用组件。

创建时间：2026-04-02
状态：已完成

---

## 定位与边界

**目标**：消除 transport.hpp 中 39 个基于 TransportMode 的 `if constexpr` 分支，用组合替代条件编译，提升可读性、可测试性和编译时间。

**In scope**：
- 从 Transport 提取 4 个独立组件：FrameProcessor、TxWorker、RxWorker、ReconnectPolicy
- 提取 TransportCore 共享状态结构体
- 创建 3 个独立 Transport 类：Transport（kThreaded）、DirectTxTransport（kDirectTx）、DirectTransport（kDirect）
- 更新 dpdk/types.hpp 和 net/socket_connect.hpp 的所有类型别名
- 更新所有测试和 benchmark
- 每个新组件有独立单元测试

**Out of scope**：
- Gateway/KillSwitch 模块迁移（独立任务）
- TLS/WebSocket 层的进一步拆分（已经独立）
- connector.hpp 瘦身（独立任务）
- C++20 modules 迁移

---

## 架构设计

### 当前问题

transport.hpp（2046 行）是一个 7 参数模板类，通过 39 个 `if constexpr` 分支和 `std::conditional_t<..., T, detail::Empty>` 模拟三种不同行为模式。这导致：
- 阅读任何方法都必须脑中 evaluate 分支条件
- 编译错误指向 `if constexpr` 内部而非调用点
- 所有模式的代码在每个编译单元全量实例化
- stats/buffer 的线程归属只靠注释约定

### 组件划分

```
已独立（不修改）：
  TlsSession            — 控制面：TLS 握手 + 密钥导出
  TlsEncryptor          — 数据面：TX 加密（RxWorker 不接触）
  TlsDecryptor          — 数据面：RX 解密（TxWorker 不接触）
  TlsRecordCrypto       — 组合：Encryptor + Decryptor
  WsFramer / RawFramer  — 无状态帧编解码
  websocket.hpp         — 无状态 WS 编解码函数
  http.hpp              — 无状态 WS 升级函数

新提取组件（4 个）：
  FrameProcessor<Framer, DeliverPolicy>
    — 拥有：WS 碎片重组缓冲区、frame filter 状态
    — 职责：帧解码、控制帧处理（ping/close/pong）、碎片重组、消息投递
    — 来源：transport_frame.hpp

  ReconnectPolicy
    — 拥有：退避状态、尝试计数
    — 职责：指数退避 + jitter 计算、尝试回调、最大次数判断
    — 来源：transport_state.hpp 中的 do_reconnect 逻辑

  TxWorker<TcpImpl, Framer, MaxPayload, QueueDepth>
    — 拥有：TX 线程、TX SPSC 队列、TX 统计、ping/pong 状态、TLS 序列号监控
    — 职责：队列消费 → 帧编码 → TLS 加密 → TCP 批量发送
    — 来源：transport_tx.hpp

  RxWorker<TcpImpl, Framer, MaxPayload, QueueDepth, RxQueueTmpl, LastOnlyDeliver>
    — 拥有：RX 线程、RX SPSC 队列、RX 统计、解密缓冲区、FrameProcessor 实例
    — 职责：TCP 轮询 → TLS 解密 → 帧处理 → 队列入队
    — 来源：transport_rx.hpp + transport_frame.hpp

新提取共享状态（1 个）：
  TransportCore<TcpImpl>
    — 拥有（跨线程共享）：tcp_, crypto_, config_, running_, reconnecting_,
      force_reconnect_, 连接元数据（tls_version_, cipher_name_ 等）
    — 不拥有（归 Worker）：stats, buffer, 线程, 队列
```

### 三个 Transport 类的组合方式

```
Transport<TcpImpl, Framer, MaxPayload, QueueDepth, RxQueueTmpl, LastOnlyDeliver>
  ├── TransportCore<TcpImpl>          （值成员）
  ├── TxWorker<TcpImpl, Framer, MaxPayload, QueueDepth>   （值成员）
  ├── RxWorker<TcpImpl, Framer, MaxPayload, QueueDepth, RxQueueTmpl, LastOnlyDeliver>  （值成员）
  └── ReconnectPolicy                 （值成员）
  API: send*(), recv*(), close_gracefully(), stats()
  线程: TX thread + RX thread

DirectTxTransport<TcpImpl, Framer, MaxPayload, QueueDepth, RxQueueTmpl, LastOnlyDeliver>
  ├── TransportCore<TcpImpl>          （值成员）
  ├── RxWorker<...>                   （值成员）
  └── ReconnectPolicy                 （值成员）
  API: send*()（直接编码+发送）, recv*(), close_gracefully(), stats()
  线程: RX thread only

DirectTransport<TcpImpl, Framer>
  ├── TransportCore<TcpImpl>          （值成员）
  ├── FrameProcessor<Framer, DirectDeliver>  （值成员）
  └── ReconnectPolicy                 （值成员）
  API: send*()（直接）, poll(), feed_rx(), process_pending(), stats()
  线程: 无
```

### 依赖方向

```
Transport / DirectTxTransport / DirectTransport
    │
    ├──→ TransportCore<TcpImpl>
    │        └──→ TcpTransport concept (eph-core)
    │        └──→ TlsRecordCrypto (eph-transport/tls_record.hpp)
    │        └──→ TransportConfig (eph-transport/transport_types.hpp)
    │
    ├──→ TxWorker / RxWorker
    │        └──→ TransportCore& (引用)
    │        └──→ FrameProcessor (RxWorker 内嵌)
    │        └──→ BoundedQueue / EvictingQueue (eph-containers)
    │
    ├──→ FrameProcessor<Framer, DeliverPolicy>
    │        └──→ MessageFramer concept (eph-core)
    │
    └──→ ReconnectPolicy
             └──→ TransportConfig& (引用，读取退避参数)
```

### 数据流

**kThreaded（Transport）发送路径**：
```
App thread: send(data) → TxWorker.enqueue(data) → [SPSC TX Queue]
TX thread:  [SPSC TX Queue] → drain → Framer.encode() → crypto_.encrypt() → tcp_.send()
```

**kThreaded（Transport）接收路径**：
```
RX thread:  tcp_.poll_rx() → crypto_.decrypt() → FrameProcessor.process()
            → DeliverPolicy(QueueDeliver) → [SPSC RX Queue]
App thread: recv() → [SPSC RX Queue] → callback(data, len, opcode)
```

**kDirect（DirectTransport）发送路径**：
```
App thread: send(data) → Framer.encode() → crypto_.encrypt() → tcp_.send()
```

**kDirect（DirectTransport）接收路径**：
```
App thread: poll() → tcp_.poll_rx() → crypto_.decrypt() → FrameProcessor.process()
            → DeliverPolicy(DirectDeliver) → on_message callback
```

---

## 接口设计

### FrameProcessor

```cpp
// 投递策略（编译时绑定，零开销）
template <typename Queue, size_t MaxPayload>
struct QueueDeliver {
    Queue& queue_;
    void operator()(const uint8_t* data, uint16_t len, uint8_t opcode,
                    uint64_t arrival_tsc, uint64_t decrypt_tsc);
};

struct DirectDeliver {
    std::function<void(const uint8_t*, uint16_t, uint8_t)>& on_message_;
    void operator()(const uint8_t* data, uint16_t len, uint8_t opcode,
                    uint64_t arrival_tsc, uint64_t decrypt_tsc);
};

template <MessageFramer Framer, typename DeliverPolicy>
class FrameProcessor {
public:
    explicit FrameProcessor(DeliverPolicy deliver, const TransportConfig& config);

    // 处理解密后的数据，返回已消费字节数
    size_t process(const uint8_t* data, size_t len);

    // 重置碎片状态（reconnect 时调用）
    void reset();

private:
    DeliverPolicy deliver_;
    const TransportConfig& config_;
    Framer framer_;
    std::vector<uint8_t> ws_frag_buf_;
    uint8_t ws_frag_opcode_{};

    // 控制帧需要发送响应——通过回调通知 Transport
    std::function<void(uint16_t code, std::string_view reason)> on_close_received_;
    std::function<void(const uint8_t* payload, uint16_t len)> on_ping_received_;
    std::function<void(const uint8_t* payload, uint16_t len)> on_pong_received_;
};
```

### TransportCore

```cpp
template <TcpTransport TcpImpl>
struct TransportCore {
    // 连接
    std::unique_ptr<TcpImpl> tcp;
    std::unique_ptr<TlsRecordCrypto> crypto;
    TcpFactory<TcpImpl> tcp_factory;
    TransportConfig config;

    // 生命周期（多线程访问）
    std::atomic<bool> running{false};
    std::atomic<bool> reconnecting{false};
    std::atomic<bool> force_reconnect{false};
    std::atomic<bool> close_requested{false};

    // 连接元数据（建立后只读）
    std::string tls_version;
    std::string cipher_name;
    std::string ws_subprotocol;
    std::string remote_ip;
    uint64_t last_handshake_ns{};
    uint64_t last_tcp_connect_ns{};
    uint64_t last_tls_handshake_ns{};
    uint64_t last_ws_upgrade_ns{};
    std::chrono::steady_clock::time_point created_at;

    // 连接建立（控制面）
    std::expected<void, ConnectionErrorInfo> do_connect();
    std::expected<void, ConnectionErrorInfo> do_ws_upgrade();
};
```

### ReconnectPolicy

```cpp
class ReconnectPolicy {
public:
    explicit ReconnectPolicy(const TransportConfig& config);

    // 执行一次重连尝试，返回是否成功
    // connect_fn: 实际连接操作（由 Transport 提供）
    // 内部处理：退避等待、尝试计数、回调通知
    bool attempt(std::function<std::expected<void, ConnectionErrorInfo>()> connect_fn);

    // 重置状态（成功连接后）
    void reset();

    // 查询
    int attempts() const;
    uint64_t total_reconnects() const;

private:
    const TransportConfig& config_;
    int attempt_{};
    uint64_t total_reconnects_{};
    std::chrono::milliseconds current_backoff_;
};
```

### TxWorker

```cpp
template <TcpTransport TcpImpl, MessageFramer Framer,
          size_t MaxPayload, size_t QueueDepth>
class TxWorker {
public:
    using TxQueue = eph::containers::BoundedQueue<TxMsg<MaxPayload>, QueueDepth>;

    explicit TxWorker(TransportCore<TcpImpl>& core);

    // 生命周期
    void start();
    void stop();

    // 发送 API（app 线程调用，入队）
    SendError enqueue(const uint8_t* data, uint16_t len, uint8_t opcode);
    SendError enqueue_for(const uint8_t* data, uint16_t len,
                          std::chrono::microseconds timeout, uint8_t opcode);
    SendError enqueue_batch(std::span<const std::span<const uint8_t>> msgs, uint8_t opcode);

    // 统计
    TxWorkerStats stats() const;
    void reset_stats();

    // 队列查询
    size_t queue_size() const;
    double queue_fill_ratio() const;
    size_t queue_hwm() const;

    // Reconnect 时由 Transport 调用
    void on_reconnected();

private:
    TransportCore<TcpImpl>& core_;
    TxQueue tx_queue_;
    std::jthread tx_thread_;

    // TX-only 状态
    ThreadStats tx_stats_;
    std::atomic<size_t> tx_hwm_{};
    bool ping_awaiting_pong_{};
    bool seq_warning_logged_{};

    // 错误通知
    std::function<void(std::string_view error)> on_error_;

    void tx_loop(std::stop_token st);
};
```

### RxWorker

```cpp
template <TcpTransport TcpImpl, MessageFramer Framer,
          size_t MaxPayload, size_t QueueDepth,
          template<typename, size_t> class RxQueueTmpl = eph::containers::BoundedQueue,
          bool LastOnlyDeliver = false>
class RxWorker {
public:
    using RxQueue = RxQueueTmpl<RxMsg<MaxPayload>, QueueDepth>;
    using FP = FrameProcessor<Framer, QueueDeliver<RxQueue, MaxPayload>>;

    explicit RxWorker(TransportCore<TcpImpl>& core);

    // 生命周期
    void start();
    void stop();

    // 接收 API（app 线程调用，从队列消费）
    bool recv(auto&& callback);
    std::optional<ReceivedMessage> try_recv();
    size_t recv_n(auto&& callback, size_t max);
    size_t drain_recv(auto&& callback);
    bool wait_recv(auto&& callback, std::chrono::microseconds timeout);

    // 统计
    RxWorkerStats stats() const;
    void reset_stats();

    // 队列查询
    size_t queue_size() const;
    double queue_fill_ratio() const;
    size_t queue_hwm() const;

    // Reconnect 时由 Transport 调用
    void on_reconnected();

private:
    TransportCore<TcpImpl>& core_;
    RxQueue rx_queue_;
    FP frame_processor_;
    std::jthread rx_thread_;

    // RX-only 状态
    ThreadStats rx_stats_;
    std::atomic<size_t> rx_hwm_{};
    bool rx_seq_warning_logged_{};

    // 延迟直方图
    eph::utils::HdrHistogram rx_latency_histogram_;
    eph::utils::HdrHistogram rx_decrypt_histogram_;
    eph::utils::HdrHistogram rx_decode_histogram_;

    // 错误通知
    std::function<void(std::string_view error)> on_error_;

    void rx_loop(std::stop_token st);
};
```

### Transport（kThreaded，主类）

```cpp
template <TcpTransport TcpImpl,
          MessageFramer Framer = WsFramer,
          size_t MaxPayload = 512,
          size_t QueueDepth = 1024,
          template<typename,size_t> class RxQueueTmpl = eph::containers::BoundedQueue,
          bool LastOnlyDeliver = false>
class Transport {
public:
    static std::expected<std::unique_ptr<Transport>, ConnectionErrorInfo>
    create(TcpFactory<TcpImpl> factory, const TransportConfig& config);

    ~Transport();

    // 发送（委托给 TxWorker）
    SendError send(const uint8_t* data, uint16_t len, uint8_t opcode = ws::kBinary);
    SendError send_text(std::string_view sv);
    SendError send_for(...);
    SendError send_close(uint16_t code = 1000, std::string_view reason = "");
    SendError send_ping(const uint8_t* payload = nullptr, uint16_t len = 0);
    SendError send_n(std::span<const std::span<const uint8_t>> msgs, uint8_t opcode = ws::kBinary);

    // 接收（委托给 RxWorker）
    bool recv(auto&& callback);
    std::optional<ReceivedMessage> try_recv();
    size_t recv_n(auto&& callback, size_t max);
    size_t drain_recv(auto&& callback);
    bool wait_recv(auto&& callback, std::chrono::microseconds timeout);

    // 生命周期
    void stop();
    bool is_running() const;
    bool is_connected() const;
    bool reconnect_now();
    bool close_gracefully(uint16_t code, std::string_view reason, std::chrono::milliseconds timeout);

    // 统计（聚合）
    TransportStats stats() const;
    void reset_stats();
    ConnectionInfo connection_info() const;

private:
    TransportCore<TcpImpl> core_;
    TxWorker<TcpImpl, Framer, MaxPayload, QueueDepth> tx_;
    RxWorker<TcpImpl, Framer, MaxPayload, QueueDepth, RxQueueTmpl, LastOnlyDeliver> rx_;
    ReconnectPolicy reconnect_;

    void handle_rx_error(std::string_view error);
    void handle_tx_error(std::string_view error);
    void do_reconnect();
};
```

### DirectTransport（kDirect）

```cpp
template <TcpTransport TcpImpl, MessageFramer Framer = WsFramer>
class DirectTransport {
public:
    static std::expected<std::unique_ptr<DirectTransport>, ConnectionErrorInfo>
    create(TcpFactory<TcpImpl> factory, const TransportConfig& config);

    ~DirectTransport();

    // 发送（直接编码 + 加密 + TCP 发送）
    SendError send(const uint8_t* data, uint16_t len, uint8_t opcode = ws::kBinary);
    SendError send_text(std::string_view sv);
    SendError send_close(uint16_t code = 1000, std::string_view reason = "");
    SendError send_ping(const uint8_t* payload = nullptr, uint16_t len = 0);

    // 接收（直接轮询）
    std::expected<uint16_t, std::string> poll();
    void feed_rx(const uint8_t* data, size_t len);
    void process_pending();

    // 生命周期
    void stop();
    bool is_running() const;
    bool is_connected() const;

    // 统计
    DirectTransportStats stats() const;

private:
    TransportCore<TcpImpl> core_;
    FrameProcessor<Framer, DirectDeliver> frame_processor_;
    ReconnectPolicy reconnect_;
    std::vector<uint8_t> decrypt_buf_;
    std::vector<uint8_t> reassembly_buf_;
    ThreadStats tx_stats_;
    ThreadStats rx_stats_;
};
```

### 类型别名更新

```cpp
// eph-dpdk/include/eph/dpdk/types.hpp
using DpdkTransport      = Transport<TcpSession<>, WsFramer, 512, 1024>;
using DpdkSmallTransport = Transport<TcpSession<>, WsFramer, 64, 256>;
using DpdkLargeTransport = Transport<TcpSession<>, WsFramer, 4096, 512>;
using DpdkEvictTransport = Transport<TcpSession<>, WsFramer, 512, 1024, EvictingQueue>;
using DpdkRawTransport   = Transport<TcpSession<>, RawFramer, 512, 1024>;

using DpdkDirectTxTransport    = DirectTxTransport<TcpSession<>, WsFramer, 512, 1024>;
using DpdkDirectTxRawTransport = DirectTxTransport<TcpSession<>, RawFramer, 512, 1024>;

using DpdkDirectTransport    = DirectTransport<TcpSession<>, WsFramer>;
using DpdkDirectRawTransport = DirectTransport<TcpSession<>, RawFramer>;

// eph-net/include/eph/net/socket_connect.hpp — 同理更新
```

### 错误体系

不变。继续使用：
- `ConnectionErrorInfo`（含 ConnectionError code + detail string + optional HTTP status）
- `SendError` 枚举
- `std::expected<T, std::string>` 用于内部操作

---

## 文件结构

```
eph-transport/include/eph/transport/
├── transport.hpp                 ← 新 Transport（kThreaded only，显著瘦身）
├── direct_tx_transport.hpp       ← 新 DirectTxTransport
├── direct_transport.hpp          ← 新 DirectTransport
├── transport_core.hpp            ← 新 TransportCore 共享状态
├── frame_processor.hpp           ← 新 FrameProcessor 组件
├── reconnect_policy.hpp          ← 新 ReconnectPolicy 组件
├── tx_worker.hpp                 ← 新 TxWorker 组件
├── rx_worker.hpp                 ← 新 RxWorker 组件
├── transport_types.hpp           ← 不变
├── presets.hpp                   ← 更新别名（删除 TransportMode 相关）
├── tls_session.hpp               ← 不变
├── tls_encryptor.hpp             ← 不变
├── tls_decryptor.hpp             ← 不变
├── tls_record.hpp                ← 不变
├── tls_constants.hpp             ← 不变
├── websocket.hpp                 ← 不变
├── ws_framer.hpp                 ← 不变
├── raw_framer.hpp                ← 不变
├── http.hpp                      ← 不变
└── detail/
    ├── transport_rx.hpp          ← 删除（逻辑迁入 rx_worker.hpp）
    ├── transport_tx.hpp          ← 删除（逻辑迁入 tx_worker.hpp）
    ├── transport_state.hpp       ← 删除（逻辑迁入 transport_core.hpp + reconnect_policy.hpp）
    ├── transport_frame.hpp       ← 删除（逻辑迁入 frame_processor.hpp）
    └── message_types.hpp         ← 保留（TxMsg/RxMsg 定义）
```

---

## 实施计划

### 实施方式：大爆炸一次替换

一次性写好所有新组件和新 Transport 类，替换旧 transport.hpp 及其 detail 文件。无中间过渡状态。

### 步骤

1. **记录 baseline**
   - 运行全部测试，确认通过
   - 运行 benchmark（bench_market, bench_pingpong），记录数据
   - 测量 transport.hpp include 的编译时间（单文件编译）
   - 推荐 skill：`/bench`

2. **实现全部新文件**（一次性创建）
   - `transport_core.hpp`：从 transport.hpp 提取共享状态 + do_connect/do_ws_upgrade
   - `frame_processor.hpp`：从 detail/transport_frame.hpp 提取，参数化 DeliverPolicy
   - `reconnect_policy.hpp`：从 detail/transport_state.hpp 提取退避逻辑
   - `tx_worker.hpp`：从 detail/transport_tx.hpp 提取，拥有 TX 队列 + 线程 + stats
   - `rx_worker.hpp`：从 detail/transport_rx.hpp 提取，拥有 RX 队列 + 线程 + stats + FrameProcessor
   - `transport.hpp`（新）：组合 TransportCore + TxWorker + RxWorker + ReconnectPolicy
   - `direct_tx_transport.hpp`：组合 TransportCore + RxWorker + ReconnectPolicy + 直接发送
   - `direct_transport.hpp`：组合 TransportCore + FrameProcessor + ReconnectPolicy + 直接收发
   - 推荐 skill：`/design auto`

3. **更新类型别名和外部引用**
   - `eph-transport/presets.hpp`
   - `eph-dpdk/include/eph/dpdk/types.hpp`
   - `eph-net/include/eph/net/socket_connect.hpp`
   - `eph-net/include/eph/net.hpp`
   - 删除 `TransportMode` 枚举

4. **更新测试**
   - `tests/net/test_transport.cpp`：适配新 Transport API（应几乎不变）
   - `tests/net/test_tcp_concept.cpp`：重写 TransportMode 编译测试
   - `tests/net/test_transport_types.cpp`：若 TransportStats 结构变化则更新
   - 新增：`tests/transport/test_frame_processor.cpp`
   - 新增：`tests/transport/test_reconnect_policy.cpp`
   - 新增：`tests/transport/test_tx_worker.cpp`（需 FakeTcpTransport）
   - 新增：`tests/transport/test_rx_worker.cpp`（需 FakeTcpTransport）
   - 推荐 skill：`/test`

5. **删除旧文件**
   - `detail/transport_rx.hpp`
   - `detail/transport_tx.hpp`
   - `detail/transport_state.hpp`
   - `detail/transport_frame.hpp`

6. **验收**
   - 全部测试通过
   - Benchmark 对比 baseline 无回归
   - 编译时间对比 baseline 有改善
   - 推荐 skill：`/bench compare`

### 验收标准

- 所有现有测试通过（test_transport, test_tcp_concept, test_transport_types, test_transport_errors）
- 所有 benchmark 无性能回归（bench_market, bench_pingpong 及 DPDK 变体）
- 每个新组件有独立单元测试（FrameProcessor, ReconnectPolicy, TxWorker, RxWorker）
- transport.hpp include 编译时间可量化改善
- 无新增 `if constexpr` 基于模式的分支（零容忍）
- Transport 的 send/recv 公共 API 对 kThreaded 用户无破坏性变更

---

## 关键决策记录

### D-1: 组合粒度——细粒度 4 组件
- **问题**：从 Transport 提取多少个独立组件
- **选项**：A. 6 个 / B. 3 个 / C. 4 个（混合）
- **决策**：A 的精化版——4 个新组件（FrameProcessor, ReconnectPolicy, TxWorker, RxWorker）
- **理由**：每个组件有明确的状态所有权和线程归属。TLS/WS 控制面已经独立，不需要再包装
- **验收标准**：每个组件可独立实例化和单元测试

### D-2: Transport 命名——保留给 kThreaded
- **问题**：三个类如何命名
- **选项**：A. Transport/DirectTxTransport/DirectTransport / B. 全部重命名 / C. tag dispatch
- **决策**：A
- **理由**：kThreaded 是主用例（所有 benchmark 和运行时测试），保留名字零改动
- **验收标准**：现有使用 `Transport<>` 的代码无需修改

### D-3: 共享状态——精简 TransportCore
- **问题**：三个类如何共享 tcp_/crypto_/config_ 等
- **选项**：A. TransportCore 结构体 / B. 各自声明 / C. back-pointer
- **决策**：A（精简版——只含跨线程共享字段，stats 归 Worker）
- **理由**：明确的"共享状态边界"，Worker 只依赖 Core 接口
- **验收标准**：TransportCore 中无 stats/buffer/线程相关字段

### D-4: FrameProcessor 投递策略——模板策略参数
- **问题**：FrameProcessor 如何投递消息到队列 vs 直接回调
- **选项**：A. 模板策略 / B. std::function / C. 函数指针
- **决策**：A（零开销，编译时绑定）
- **理由**：RX hot loop 每帧调用一次 deliver，间接调用在 HFT 场景不可接受
- **验收标准**：deliver 调用在编译后为直接函数调用（可通过 objdump 验证）

### D-5: Reconnect 编排——Transport 层协调
- **问题**：谁负责 reconnect 的 stop-worker → rebuild-core → restart-worker 流程
- **选项**：A. Transport 编排 / B. RxWorker 自处理 / C. 事件驱动
- **决策**：A
- **理由**：Reconnect 涉及 TCP + TLS + WS 三层重建，天然是编排逻辑
- **验收标准**：RxWorker/TxWorker 不包含连接建立代码，只通过 on_error_ 回调通知

### D-6: Stats 归属——各组件拥有自己的 stats
- **问题**：统计数据由谁持有
- **选项**：A. 各组件拥有 / B. 统一结构体
- **决策**：A
- **理由**：消除跨线程读写竞争，TxWorker 写自己的 stats，RxWorker 写自己的
- **验收标准**：Transport::stats() 只做聚合，不持有 stats 字段

### D-7: 实施方式——大爆炸
- **问题**：渐进式提取 vs 一次替换
- **选项**：A. 自底向上 / B. 自顶向下 / C. 大爆炸
- **决策**：C
- **理由**：kDirect/kDirectTx 无运行时使用者，不需要中间过渡状态。一次到位避免适配层复杂性
- **验收标准**：替换完成后无旧 TransportMode 枚举残留

### D-8: 旧 API 兼容——硬切
- **问题**：旧 Transport<..., kDirectTx, ...> 用法如何过渡
- **选项**：A. 直接删除 / B. deprecated wrapper
- **决策**：A
- **理由**：kDirect/kDirectTx 只在编译测试中使用，无需过渡
- **验收标准**：编译无 deprecated 警告
