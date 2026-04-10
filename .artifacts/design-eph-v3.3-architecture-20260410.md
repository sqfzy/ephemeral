# eph 架构 v3.3 — Final Design Spec

## Context
- 时间：2026-04-10 12:43
- 分支：refactor/transport-api
- 基于 commit：54b056f
- 状态：**FROZEN**（所有架构决议锁定，可以进入实施阶段）
- 上游讨论：
  - `.artifacts/discuss-20260410-111612.md` (5 角色 4 轮：variant 收敛)
  - `.artifacts/discuss-20260410-114659.md` (5 角色 12 轮：v1.6 架构基础)
  - 后续 4 轮交互式细化：v1.6 → v2.0 → v3.0 → v3.1 → v3.2 → **v3.3**

## 设计哲学

> **eph = 内核 narrow-waist × C++23 模板单态化 × Tokio 命名/分层 × HFT 零拷贝兑现**

- **3 个 narrow concept**（Stream / Codec / Poller）建立窄腰，让后端、编解码器、多路复用器各自插拔
- **`TcpStream<C, Tls>` / `UdpSocket<C>`** 是用户面唯一的 per-connection 类型，融合协议栈状态
- **`Poller`** 是唯一的 I/O 驱动器，单 / 多连接走同一 API
- **PacketView** associated type 实现 zero-copy in-place TLS decrypt
- **Tokio 命名**：TcpStream/UdpSocket/Poller 与 `tokio::net::*`、`mio::Poll` 一致
- **物理隔离 kernel/DPDK**：模块切法 B，CI 机器永远不需要装 DPDK

## 模块结构（切法 B，11 个模块）

```
eph-utils                 [不变] 时间 / 日志 / 格式化
eph-containers            [不变] SPSC / EvictingQueue / RingBuffer
eph-core                  [瘦身] Codec concept + ErrorInfo + Error enum
eph-codec                 [新]   codec 实现（WsCodec, RawCodec, LengthPrefixCodec, Mold64Codec, ...）
eph-net                   [新]   网络 concepts + SocketAddr + ReconnectPolicy + 测试 mocks
eph-net-kernel            [新]   epoll-based: KernelTcpStream / KernelUdpSocket / KernelPoller
eph-net-dpdk              [新]   DPDK-based: DpdkTcpStream / DpdkUdpSocket / DpdkPoller + EAL + 基础设施
eph-fix                   [不变] FIX framer，满足 eph::core::Codec concept
eph-itch                  [不变] ITCH framer，满足 eph::core::Codec concept
eph-json                  [不变] JSON framer，满足 eph::core::Codec concept
eph-book                  [不变] 订单簿，跨 itch/json bridge

DELETED:
  eph-transport           → 内容下沉到 eph-net-{kernel,dpdk}，命名收敛
  eph-dpdk                → 改名为 eph-net-dpdk
```

### 模块依赖图

```
                         eph-utils
                            |
                            v
                       eph-containers
                            |
                +-----------+-----------+
                |                       |
                v                       v
             eph-core              parsers:
            (Codec concept,    eph-fix, eph-itch,
             ErrorInfo)        eph-json, eph-book
                |              (依赖 eph-core 的 Codec concept)
        +-------+-------+
        |               |
        v               v
    eph-codec        eph-net
   (codec impls)  (concepts +
                   SocketAddr +
                   ReconnectPolicy +
                   FakeStream +
                   TestPoller)
                       |
              +--------+--------+
              |                 |
              v                 v
        eph-net-kernel    eph-net-dpdk
        (epoll +          (lcore +
         KernelTcp/       DpdkTcp/Udp +
         Udp/Poller)      Poller +
                          Eal + ...)
```

**关键约束**：
- eph-net **不依赖** eph-codec：codec 是模板参数，由用户在自己的 target 里同时 link
- eph-net-{kernel,dpdk} 之间互不依赖：用户根据需要选一个 link
- eph-fix/itch/json/book 永远不依赖任何 eph-net*：parser 模块只需要 Codec concept
- DPDK 的所有重型 build 依赖（vfio、hugepages、`apply_dpdk_pmd_linkgroups()`）只在 eph-net-dpdk 模块内，kernel-only 用户完全免疫

---

## 完整 Concept 定义

### eph-core/include/eph/core/error.hpp

```cpp
namespace eph::core {

enum class Error : uint8_t {
    Ok = 0,
    
    // 连接生命周期
    ConnectFailed,
    Disconnected,
    Timeout,
    NotAttached,        // ★ Stream/Socket 未 attach 到 Poller 时调 send
    
    // TLS
    TlsHandshakeFailed,
    TlsRecordBad,
    TlsCipherFailed,
    
    // WebSocket
    WsHandshakeFailed,
    WsFrameBad,
    WsCloseReceived,
    
    // Codec / 协议
    CodecNeedMoreData,  // 流式 codec：返回 None 等更多数据时的内部信号
    CodecBad,
    CodecOverflow,
    
    // I/O
    WouldBlock,
    NoData,
    BufferFull,
    
    // 内部
    InvalidConfig,
    OutOfMemory,
};

struct ErrorInfo {
    Error       code;
    const char* detail;   // string literal 或 static storage，永不悬垂
    
    constexpr ErrorInfo(Error c, const char* d = "") noexcept
        : code(c), detail(d) {}
};

constexpr const char* error_name(Error e) noexcept;

} // namespace eph::core
```

### eph-core/include/eph/core/codec.hpp

```cpp
namespace eph::core {

// PacketView is an associated type provided by each Stream/UdpSocket impl.
// Codec is templated to accept any PacketView meeting these requirements:
//
//   uint8_t* writable_data() noexcept;
//   const uint8_t* data() const noexcept;
//   size_t length() const noexcept;
//   void trim_front(size_t n) noexcept;   // skb_pull equivalent
//   void trim_back(size_t n) noexcept;    // skb_trim equivalent
//   uint64_t arrival_tsc() const noexcept;

// OutputBuffer 给 codec 注入自动响应（WS pong/close）的写入 sink
class OutputBuffer {
public:
    std::expected<void, ErrorInfo> append(const uint8_t* data, size_t len);
    std::expected<void, ErrorInfo> reserve(size_t n);
    uint8_t* writable_tail(size_t n);  // zero-copy append
    void commit(size_t n);
    size_t available() const noexcept;
};

// ─── Stream Codec (TCP-style，增量解码) ────────────────────────
template <class T>
concept StreamCodec = requires(T& t, typename T::PacketViewRef view,
                                OutputBuffer& out, uint8_t* enc_buf,
                                size_t enc_cap) {
    typename T::Frame;
    typename T::PacketViewRef;  // 通常是 PacketView&
    
    // decode is stateful (T& not const)
    // - Ok(Some(frame))    : 解出一帧（partial bytes 可能未消费，留给下次）
    // - Ok(None)           : 数据不够，等更多
    // - Err(ErrorInfo)     : 协议错误
    //
    // out 给 codec 注入自动响应（WsCodec 看到 ping → 写 pong → 返回 None）
    { t.decode(view, out) } -> std::same_as<
        std::expected<std::optional<typename T::Frame>, ErrorInfo>>;
    
    { t.encode(enc_buf, enc_cap, std::declval<typename T::Frame>()) }
        -> std::same_as<std::expected<size_t, ErrorInfo>>;
    
    { T::max_overhead } -> std::convertible_to<size_t>;
    { T::is_streaming } -> std::convertible_to<bool>;
};

// ─── Datagram Codec (UDP-style，每包独立) ──────────────────────
template <class T>
concept DatagramCodec = requires(T& t,
                                  typename T::PacketViewRef dgram,
                                  OutputBuffer& out,
                                  std::function<void(typename T::Frame)> sink,
                                  uint8_t* enc_buf, size_t enc_cap) {
    typename T::Frame;
    typename T::PacketViewRef;
    
    // 处理一个完整 datagram，可能 emit 0/1/N 帧（MoldUDP64：一个 packet 含多个 ITCH msg）
    // 必须消费整个 datagram（不像 stream codec 可以剩余）
    // sink 是 codec 内部回调，每解出一帧调一次
    { t.decode(dgram, out, sink) } -> std::same_as<
        std::expected<size_t, ErrorInfo>>;  // 返回 emit 的帧数
    
    { t.encode(enc_buf, enc_cap, std::declval<typename T::Frame>()) }
        -> std::same_as<std::expected<size_t, ErrorInfo>>;
    
    { T::max_overhead } -> std::convertible_to<size_t>;
    { T::is_streaming } -> std::convertible_to<bool>;  // false
};

// ─── 通用 Codec 概念 ──────────────────────────────────────────
template <class T>
concept Codec = StreamCodec<T> || DatagramCodec<T>;

} // namespace eph::core
```

### eph-net/include/eph/net/concepts.hpp

```cpp
namespace eph::net {

// ─── SocketAddr ───────────────────────────────────────────────
struct Ipv4Addr {
    uint8_t octets[4];
    constexpr Ipv4Addr() noexcept;
    constexpr explicit Ipv4Addr(uint32_t be) noexcept;
    static std::expected<Ipv4Addr, core::ErrorInfo> parse(std::string_view) noexcept;
    std::string to_string() const;
};

struct SocketAddr {
    Ipv4Addr ip;
    uint16_t port;
    
    constexpr SocketAddr() noexcept = default;
    constexpr SocketAddr(Ipv4Addr a, uint16_t p) noexcept : ip(a), port(p) {}
};

// ─── TcpState ─────────────────────────────────────────────────
enum class TcpState : uint8_t {
    Closed, Listen, SynSent, SynReceived, Established,
    FinWait1, FinWait2, CloseWait, Closing, LastAck, TimeWait,
};

constexpr const char* tcp_state_name(TcpState) noexcept;

// ─── Pollable concept (Poller 内部使用) ───────────────────────
// Stream 和 Datagram 都隐含满足这个 concept
template <class T>
concept Pollable = requires(T& t) {
    typename T::PacketView;
    
    // friend Poller 通过这些方法驱动 T:
    // - poll_once() :  执行一次 I/O，返回处理的 packet 数
    // - is_attached_() :  是否已 attach
    // - native_handle() :  kernel: int fd; dpdk: void* (mbuf 池或 conn id)
    // 这些方法应为 private + friend Poller，但为了 concept check 暴露
};

// ─── Stream concept (TCP-style，连接) ─────────────────────────
template <class T>
concept Stream = Pollable<T> && requires(T& t,
                                          std::span<const uint8_t> data) {
    typename T::CodecType;
    typename T::OnMessage;       // std::function<void(const uint8_t*, uint16_t)>
    
    { t.send(data) }            -> std::same_as<std::expected<size_t, core::ErrorInfo>>;
    { t.close_gracefully() }    -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { t.is_attached() }         -> std::same_as<bool>;
    { t.state() }               -> std::same_as<TcpState>;
    { t.on_message }            -> std::convertible_to<typename T::OnMessage>;
};

// ─── Datagram concept (UDP-style，无连接) ─────────────────────
template <class T>
concept Datagram = Pollable<T> && requires(T& t,
                                            std::span<const uint8_t> data,
                                            const SocketAddr& dst,
                                            const SocketAddr& mcast_group) {
    typename T::CodecType;
    typename T::OnDatagram;       // void(const uint8_t*, uint16_t, const SocketAddr&)
    
    { t.send_to(data, dst) }    -> std::same_as<std::expected<size_t, core::ErrorInfo>>;
    { t.join_multicast(mcast_group) } -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { t.leave_multicast(mcast_group) } -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { t.is_attached() }         -> std::same_as<bool>;
    { t.on_datagram }           -> std::convertible_to<typename T::OnDatagram>;
};

// ─── Poller concept ───────────────────────────────────────────
template <class T>
concept Poller = requires(T& p, Pollable auto* obj,
                           std::chrono::milliseconds to) {
    { p.add(obj) }    -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { p.remove(obj) } -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { p.poll() }      -> std::convertible_to<size_t>;
    // poll(timeout) 是可选 overload (kernel 有，DPDK 无)
};

} // namespace eph::net
```

### eph-net/include/eph/net/reconnect_policy.hpp

```cpp
namespace eph::net {

class ReconnectPolicy {
    std::chrono::milliseconds initial_backoff_;
    std::chrono::milliseconds max_backoff_;
    double                    multiplier_;
    double                    jitter_factor_;
    uint32_t                  max_attempts_;
    uint32_t                  attempts_;
public:
    explicit ReconnectPolicy(ReconnectPolicyConfig cfg);
    
    bool should_reconnect() const noexcept;
    std::chrono::milliseconds next_backoff() noexcept;
    void reset() noexcept;
};

} // namespace eph::net
```

### eph-net/include/eph/net/test/fake_stream.hpp

```cpp
namespace eph::net::test {

// 满足 Stream concept 的内存内 mock，无系统调用
class FakeStream {
    // ... internal byte buffer for inject/collect
public:
    using CodecType = void;  // no codec
    using OnMessage = std::function<void(const uint8_t*, uint16_t)>;
    
    static std::unique_ptr<FakeStream> create();
    
    OnMessage on_message;
    
    // 测试控制 API
    void inject_rx(std::span<const uint8_t> data);
    std::span<const uint8_t> collect_tx() const;
    void clear_tx();
    
    // Stream concept 实现
    std::expected<size_t, core::ErrorInfo> send(std::span<const uint8_t> data);
    std::expected<void, core::ErrorInfo>   close_gracefully();
    bool is_attached() const noexcept;
    TcpState state() const noexcept;
};

class FakeDatagram { /* 类似，满足 Datagram concept */ };

template <Pollable P>
class TestPoller {
    std::vector<P*> registered_;
public:
    static std::unique_ptr<TestPoller> create();
    
    std::expected<void, core::ErrorInfo> add(P* p);
    std::expected<void, core::ErrorInfo> remove(P* p);
    size_t poll() noexcept;  // 立即驱动所有 registered.poll_once()
};

} // namespace eph::net::test
```

---

## Backend 实现类

### eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp

```cpp
namespace eph::net::kernel {

template <core::StreamCodec C, bool EnableTls = true>
class TcpStream {
public:
    using CodecType  = C;
    using PacketView = detail::SpanView;  // contiguous span<const uint8_t>
    using OnMessage  = std::function<void(const uint8_t*, uint16_t)>;
    
    static std::expected<std::unique_ptr<TcpStream>, core::ErrorInfo>
    create(StreamConfig cfg);  // 同步：TCP connect + TLS handshake + WS upgrade
    
    // 用户面 API
    OnMessage on_message;
    
    std::expected<size_t, core::ErrorInfo> send(std::span<const uint8_t> data);
    std::expected<void, core::ErrorInfo>   close_gracefully();
    bool      is_attached() const noexcept { return attached_to_ != nullptr; }
    TcpState  state() const noexcept;
    int       fd() const noexcept { return sock_.fd(); }
    
    ~TcpStream();  // 自动 detach if attached
    
private:
    template <class P> friend class Poller;
    
    // Poller 通过 friend 调用：
    size_t poll_once_() noexcept;            // 内部 recv → TLS decrypt → codec.decode → on_message
    void notify_attached_(Poller* p) noexcept;
    void notify_detached_() noexcept;
    void notify_reconnect_() noexcept;       // 重连后 fd 变了，通知 Poller 重新 epoll_ctl
    
    detail::KernelByteSocket sock_;
    C                        codec_;
    [[no_unique_address]] std::conditional_t<EnableTls,
                                              detail::TlsState,
                                              std::monostate> tls_;
    detail::ReassemblyBuffer reasm_;
    Poller*                  attached_to_ = nullptr;
    ReconnectPolicy          reconnect_policy_;
};

} // namespace eph::net::kernel
```

### eph-net-kernel/include/eph/net/kernel/udp_socket.hpp

```cpp
namespace eph::net::kernel {

template <core::DatagramCodec C>
class UdpSocket {
public:
    using CodecType    = C;
    using PacketView   = detail::SpanView;
    using OnDatagram   = std::function<void(const uint8_t*, uint16_t, const SocketAddr&)>;
    
    static std::expected<std::unique_ptr<UdpSocket>, core::ErrorInfo>
    create(UdpConfig cfg);
    
    OnDatagram on_datagram;
    
    std::expected<size_t, core::ErrorInfo> send_to(std::span<const uint8_t>,
                                                    const SocketAddr&);
    std::expected<void, core::ErrorInfo>   join_multicast(const SocketAddr&);
    std::expected<void, core::ErrorInfo>   leave_multicast(const SocketAddr&);
    
    // 可选 connected 模式（filter 来源）
    std::expected<void, core::ErrorInfo>   connect_to(const SocketAddr&);
    
    bool is_attached() const noexcept;
    int  fd() const noexcept;
    
    ~UdpSocket();
    
private:
    template <class P> friend class Poller;
    size_t poll_once_() noexcept;  // recvmsg → codec.decode → on_datagram
    
    detail::KernelUdpSocket sock_;
    C                       codec_;
    Poller*                 attached_to_ = nullptr;
};

} // namespace eph::net::kernel
```

### eph-net-kernel/include/eph/net/kernel/poller.hpp

```cpp
namespace eph::net::kernel {

class Poller {
public:
    static std::expected<std::unique_ptr<Poller>, core::ErrorInfo>
    create(PollerConfig cfg = {});
    
    template <Pollable P>
    std::expected<void, core::ErrorInfo> add(P* obj);
    
    template <Pollable P>
    std::expected<void, core::ErrorInfo> remove(P* obj);
    
    // 非阻塞
    size_t poll() noexcept;
    // epoll_wait with timeout
    size_t poll(std::chrono::milliseconds timeout) noexcept;
    
    ~Poller();
    
private:
    // P2: type erase via function pointer
    struct PollableEntry {
        void*    obj;
        size_t (*poll_fn)(void*);
        int      fd;
    };
    
    int                          epoll_fd_;
    std::vector<PollableEntry>   entries_;
    std::array<epoll_event, 64>  events_buf_;
};

} // namespace eph::net::kernel
```

### eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp

```cpp
namespace eph::net::dpdk {

template <core::StreamCodec C, bool EnableTls = true>
class TcpStream {
public:
    using CodecType  = C;
    using PacketView = detail::MbufView;  // mbuf-backed，支持零拷贝 in-place mutation
    using OnMessage  = std::function<void(const uint8_t*, uint16_t)>;
    
    static std::expected<std::unique_ptr<TcpStream>, core::ErrorInfo>
    create(StreamConfig cfg);
    
    OnMessage on_message;
    
    std::expected<size_t, core::ErrorInfo> send(std::span<const uint8_t> data);
    std::expected<void, core::ErrorInfo>   close_gracefully();
    bool                                    is_attached() const noexcept;
    TcpState                                state() const noexcept;
    
    ~TcpStream();
    
private:
    template <core::Pollable P> friend class Poller;
    
    void process_burst_(rte_mbuf** mbufs, uint16_t n, uint64_t rx_tsc) noexcept;
    
    detail::DpdkTcpSession sess_;       // 当前 TcpSession 改名
    C                      codec_;
    [[no_unique_address]] std::conditional_t<EnableTls,
                                              detail::TlsState,
                                              std::monostate> tls_;
    detail::MbufReassembly reasm_;       // mbuf chain reassembly
    Poller*                attached_to_ = nullptr;
    ReconnectPolicy        reconnect_policy_;
};

} // namespace eph::net::dpdk
```

### eph-net-dpdk/include/eph/net/dpdk/poller.hpp

```cpp
namespace eph::net::dpdk {

template <core::Pollable P = void>  // void = type-erased mode
class Poller {
public:
    static std::expected<std::unique_ptr<Poller>, core::ErrorInfo>
    create(PollerConfig cfg);
    
    template <Pollable T>
    std::expected<void, core::ErrorInfo> add(T* obj);
    
    template <Pollable T>
    std::expected<void, core::ErrorInfo> remove(T* obj);
    
    // 非阻塞 lcore burst poll
    size_t poll() noexcept;
    
    ~Poller();
    
private:
    // P2: type erase via function pointer for heterogeneous Pollables
    struct PollableEntry {
        void*    obj;
        void   (*process_burst_fn)(void*, rte_mbuf**, uint16_t, uint64_t);
        uint32_t conn_id;  // 5-tuple → entry index 的 hash key
    };
    
    detail::FlowSteeringTable           routing_;
    std::array<PollableEntry, kMaxConn> entries_;
    size_t                              n_entries_ = 0;
    uint16_t                            port_id_;
    uint16_t                            queue_id_;
};

} // namespace eph::net::dpdk
```

### eph-net-dpdk 其它文件

```
eph-net-dpdk/include/eph/net/dpdk/
    tcp_stream.hpp
    udp_socket.hpp
    poller.hpp
    eal.hpp                  (RAII EAL init/teardown)
    flow_steering.hpp        (RSS / RTE flow rules)
    udp_packet_template.hpp  (预计算 UDP+IP 头模板)
    multicast.hpp            (多播组管理 helper)
    arp.hpp, dns.hpp         (链路 helper)
    detail/
        dpdk_tcp_session.hpp (TCP state machine)
        dpdk_udp_socket.hpp
        mbuf_view.hpp
        mbuf_reassembly.hpp
        ...
```

---

## Codec 实现示例

### eph-codec/include/eph/codec/ws_codec.hpp

```cpp
namespace eph::codec {

class WsCodec {
public:
    using Frame         = std::span<const uint8_t>;
    using PacketViewRef = core::PacketView&;  // 通用约束
    
    static constexpr size_t max_overhead = 14;     // WS frame header
    static constexpr bool   is_streaming = true;
    
    // Stateful: 持有重组缓冲区 + control frame 状态机
    explicit WsCodec(WsCodecConfig cfg);
    
    // 增量 decode：返回一帧（可能空）+ 自动响应已写入 out
    template <class PacketView>
    std::expected<std::optional<Frame>, core::ErrorInfo>
    decode(PacketView& view, core::OutputBuffer& out);
    
    // encode 一个 application frame
    std::expected<size_t, core::ErrorInfo>
    encode(uint8_t* buf, size_t cap, Frame payload);
    
private:
    detail::WsFrameAssembler reasm_;
    detail::WsControlFsm     ctrl_;  // 处理 ping/pong/close 自动响应
};

} // namespace eph::codec
```

### eph-codec/include/eph/codec/mold64_codec.hpp

```cpp
namespace eph::codec {

class Mold64Codec {
public:
    struct Frame {
        uint64_t seq_num;
        std::span<const uint8_t> payload;  // 一条 ITCH msg
    };
    using PacketViewRef = core::PacketView&;
    
    static constexpr size_t max_overhead = 20;
    static constexpr bool   is_streaming = false;  // datagram codec
    
    explicit Mold64Codec(Mold64Config cfg);
    
    // 处理一个完整 datagram，emit 0/1/N 个 ITCH msg via sink
    template <class PacketView>
    std::expected<size_t, core::ErrorInfo>
    decode(PacketView& dgram, core::OutputBuffer& out,
           std::function<void(Frame)> sink);
    
    std::expected<size_t, core::ErrorInfo>
    encode(uint8_t* buf, size_t cap, Frame frame);
    
    uint64_t expected_seq() const noexcept;
    uint64_t gap_count() const noexcept;
    
private:
    uint64_t expected_seq_ = 1;
    uint64_t gaps_ = 0;
};

} // namespace eph::codec
```

---

## 用户代码示例

### Example 1: kernel-only 用户（CI / 测试机 / 开发机）

```cpp
// xmake.lua
add_requires("eph", { configs = { dpdk = false } })
target("my_test_client")
    set_kind("binary")
    add_files("main.cpp")
    add_packages("eph")
    add_deps("eph-net-kernel", "eph-codec")
```

```cpp
// main.cpp
#include <eph/net/concepts.hpp>
#include <eph/net/kernel/tcp_stream.hpp>
#include <eph/net/kernel/poller.hpp>
#include <eph/codec/ws_codec.hpp>

namespace en = eph::net::kernel;
namespace ec = eph::codec;

int main() {
    auto poller = en::Poller::create({}).value();
    
    auto stream = en::TcpStream<ec::WsCodec>::create({
        .remote_host = "ws.binance.com",
        .remote_port = 443,
        .ws_path = "/ws/btcusdt@trade",
    }).value();
    
    stream->on_message = [](const uint8_t* data, uint16_t len) {
        spdlog::info("market data: {} bytes", len);
    };
    
    poller->add(stream.get()).value();
    
    while (running) {
        poller->poll(100ms);
    }
}
```

### Example 2: HFT 生产用户（DPDK + TCP 下单 + UDP 行情）

```cpp
// xmake.lua
add_requires("eph", { configs = { dpdk = true } })
target("hft_strategy")
    set_kind("binary")
    add_files("main.cpp")
    add_packages("eph")
    add_deps("eph-net-dpdk", "eph-codec", "eph-fix", "eph-itch")
    apply_dpdk_pmd_linkgroups()
```

```cpp
// main.cpp
#include <eph/net/dpdk/tcp_stream.hpp>
#include <eph/net/dpdk/udp_socket.hpp>
#include <eph/net/dpdk/poller.hpp>
#include <eph/net/dpdk/eal.hpp>
#include <eph/codec/ws_codec.hpp>
#include <eph/codec/mold64_codec.hpp>

namespace en = eph::net::dpdk;
namespace ec = eph::codec;

int main(int argc, char** argv) {
    en::Eal eal{argc, argv};  // RAII DPDK EAL init
    
    // 一个 Poller 同时管 TCP（下单）+ UDP（行情）
    auto poller = en::Poller<>::create({
        .port_id = 0,
        .queue_id = 0,
        .lcore = 4,
    }).value();
    
    // ── TCP：下单通道 ─────────────────────────
    auto order_ch = en::TcpStream<ec::WsCodec>::create({
        .remote_host = "fix.binance.com",
        .remote_port = 443,
    }).value();
    order_ch->on_message = [](auto* d, auto len) {
        handle_exec_report(d, len);
    };
    poller->add(order_ch.get()).value();
    
    // ── UDP：CME ITCH multicast ─────────────────
    auto md_sock = en::UdpSocket<ec::Mold64Codec>::create({
        .bind_addr = SocketAddr{Ipv4Addr{0,0,0,0}, 30000},
    }).value();
    md_sock->on_datagram = [](auto* d, auto len, const auto& src) {
        process_market_data(d, len);
    };
    md_sock->join_multicast(SocketAddr{Ipv4Addr{233,54,12,111}, 30001}).value();
    poller->add(md_sock.get()).value();
    
    // ── 单 loop 同时驱动 TCP + UDP ──────────────
    while (running) {
        poller->poll();
    }
}
```

### Example 3: 嵌入用户已有 main loop

```cpp
// 用户已有 trading framework 的 main loop
while (user_running) {
    user_app.tick();
    poller->poll(0ms);  // 非阻塞，融入现有 loop
}
```

### Example 4: 单元测试（codec 独立测试）

```cpp
#include <eph/net/test/fake_stream.hpp>
#include <eph/net/test/test_poller.hpp>
#include <eph/codec/ws_codec.hpp>

namespace ent = eph::net::test;

TEST(WsCodec, ParsesPingFrameAndAutoResponds) {
    auto poller = ent::TestPoller<ent::FakeStream>::create();
    auto fake = ent::FakeStream::create();
    poller->add(fake.get());
    
    fake->inject_rx({0x89, 0x00});  // WS ping frame, no payload
    
    poller->poll();
    
    auto tx = fake->collect_tx();
    EXPECT_EQ(tx.size(), 2);
    EXPECT_EQ(tx[0], 0x8A);  // WS pong frame opcode
}
```

---

## 重构 Phase 划分（v3.3）

每个 phase 是一组独立 commit，可以单独 review/revert。

### Phase 0: eph-core 瘦身
**目标**：把网络专属概念从 eph-core 移出  
**改动**：
- 删除 `eph-core/include/eph/core/tcp_concept.hpp`（TcpTransport concept 整体下移到 eph-net）
- 删除 `eph-core/include/eph/core/fake_tcp_transport.hpp`（FakeTcpTransport 移到 eph-net/test/）
- 新增 `eph-core/include/eph/core/error.hpp`（Error enum + ErrorInfo）
- 新增 `eph-core/include/eph/core/codec.hpp`（StreamCodec + DatagramCodec concept）
- 旧的 `transport_errors.hpp` 等收敛到 `error.hpp`

**风险**：低  
**破坏面**：所有 import `eph/core/tcp_concept.hpp` 的文件都要改 include。grep 显示 14 个文件。

### Phase 1: 创建 eph-codec 模块
**目标**：把 framer 迁移到 eph-codec，引入 stateful Codec 接口  
**改动**：
- 新建 `eph-codec/` 模块
- 把 `eph-transport/include/eph/transport/{ws_framer,raw_framer,length_prefix_framer}.hpp` 移到 `eph-codec/include/eph/codec/`
- 改造接口：`decode(view, OutputBuffer&) -> expected<optional<Frame>>`（stateful + auto-response）
- WsCodec 内部吃下原 Channel 类的 control frame 处理逻辑
- 新增 `Mold64Codec`（接管 `eph-itch/moldudp64.hpp` 的功能或封装它）
- 新增 `RawDatagramCodec`

**风险**：中（codec 接口变化影响所有 framer 用户）  
**破坏面**：所有 framer 实现 + 用户代码

### Phase 2: 创建 eph-net 模块
**目标**：建立网络 concept 和共享类型  
**改动**：
- 新建 `eph-net/` 模块（注意：与现有 eph-net 同名，需要先 rename old → eph-net-legacy 或类似）
- 创建 `eph-net/include/eph/net/concepts.hpp`（Stream/Datagram/Pollable/Poller concepts）
- 创建 `socket_addr.hpp`、`reconnect_policy.hpp`（从 eph-transport 迁移）
- 创建 `test/fake_stream.hpp`、`test/test_poller.hpp`、`test/fake_datagram.hpp`
- 对应单元测试

**风险**：中  
**破坏面**：模块名冲突需要先解决

### Phase 3: 创建 eph-net-kernel 模块
**目标**：把 kernel-side 实现迁过来并改名  
**改动**：
- 新建 `eph-net-kernel/` 模块
- 把 `eph-net/include/eph/net/socket_transport.hpp` 等改造为 `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp`
- 重命名 `SocketTransport` → `KernelTcpStream`
- 实现 `KernelUdpSocket`（新功能，eph-net 当前可能没有 kernel UDP）
- 实现 `KernelPoller`（epoll-based）
- 完整的单元测试 + 集成测试

**风险**：高（KernelPoller 是新代码）  
**破坏面**：所有原 eph-net 的下游用户

### Phase 4: 创建 eph-net-dpdk 模块
**目标**：把 DPDK-side 实现迁过来并改名  
**改动**：
- 新建 `eph-net-dpdk/` 模块
- 把 `eph-dpdk/include/eph/dpdk/*` 全部迁移到 `eph-net-dpdk/include/eph/net/dpdk/`
- 重命名 `TcpSession` → `DpdkTcpStream`
- 重命名 `Reactor` → `DpdkPoller`，改造为支持异质 Pollable（P2 函数指针 type erase）
- 实现 `DpdkUdpSocket`（封装现有 UdpSender/接收逻辑）
- 完整的单元测试 + DPDK e2e 测试

**风险**：高（Reactor → Poller 接口变化大）  
**破坏面**：所有 eph-dpdk 用户

### Phase 5: PacketView 引入
**目标**：实现 zero-copy in-place TLS decrypt 路径  
**改动**：
- 在 `eph-net/include/eph/net/concepts.hpp` 定义 `PacketView` 接口要求
- DpdkTcpStream 暴露 `MbufView`（mbuf-backed，零拷贝）
- KernelTcpStream 暴露 `SpanView`（contiguous span）
- 改造 TLS decrypt 为 in-place（aws-lc 支持）
- bench 验证 `lat_ex_market` RX latency 不退化且最好下降

**风险**：中（性能验证点）  
**破坏面**：仅实现层

### Phase 6: 更新 examples / tests / benchmarks
**改动**：
- 所有 `examples/*.cpp` 改用新 API
- `tests/integration/*.cpp` 改用新 API
- `benchmarks/latency/*` 改用新 API
- 各模块自带的 `tests/`、`benchmarks/`、`fuzzers/` 跟新

**风险**：中  
**破坏面**：大量 lockstep 改动

### Phase 7: 删除旧模块
**改动**：
- 删除 `eph-transport/`
- 删除 `eph-dpdk/`
- 删除任何 deprecation alias
- 更新根 `xmake.lua`

**风险**：中  
**破坏面**：清理

### Phase 8: 文档更新
**改动**：
- 重写 `CLAUDE.md`（模块依赖图、命名约定）
- 重写 `README.md`、`summary.md`
- 重新生成所有 per-module 文档（README/CHANGELOG/summary/ONBOARDING）
- 删除 `docs/reactor-guide.md`，新增 `docs/poller-guide.md`、`docs/architecture.md`

**风险**：低  
**破坏面**：文档

### Phase 顺序约束

```
Phase 0  →  Phase 1  →  Phase 2  →  Phase 3  ──┐
                              │                 ├──→  Phase 5  →  Phase 6  →  Phase 7  →  Phase 8
                              └──→  Phase 4  ───┘
```

Phase 3 和 Phase 4 可以**并行**（独立模块）。其它必须串行。

预计总周期（assuming 1 dev full-time）：
- Phase 0: 0.5 day
- Phase 1: 2 days
- Phase 2: 1 day
- Phase 3: 3 days
- Phase 4: 4 days（DPDK 测试设置复杂）
- Phase 5: 2 days
- Phase 6: 2 days
- Phase 7: 0.5 day
- Phase 8: 1 day
- **总计：~16 days**

---

## 验证点 (ship-readiness)

每个 phase 完成的衡量标准：

1. **所有 phase**：CI 全绿（`xmake build -g tests && xmake run -g tests`）
2. **Phase 5**：`lat_ex_market` benchmark 的 P50/P99 RX latency **不退化**，理想下降 1-2μs（PacketView 节省 memcpy）
3. **Phase 6**：所有 examples 编译并能跑通烟测
4. **Phase 7**：`grep -r "eph-transport\|eph-dpdk\b\|TcpTransport\|MessageFramer\|TcpSession\|SocketTransport\|class Channel\|Reactor" --include="*.hpp" --include="*.cpp"` 无业务代码命中（只剩文档/历史 artifacts）
5. **Phase 8**：新人 onboarding 跑 README quickstart 5 分钟内能跑通 `simple_hft_dpdk_reactor.cpp`（需要 DPDK 环境）或 `simple_hft.cpp`（kernel）

---

## 命名总表

| 旧名 | 新名 | 所在模块 |
|---|---|---|
| `concept TcpTransport` | `concept Stream` | eph-net |
| `concept MessageFramer` | `concept StreamCodec` / `concept DatagramCodec` | eph-core |
| `class SocketTransport` | `class KernelTcpStream<C, Tls>` | eph-net-kernel |
| `class TcpSession<>` | `class DpdkTcpStream<C, Tls>` | eph-net-dpdk |
| `class Transport<TcpImpl, Framer, ...>` | **删除**（并入对应 backend Stream） | — |
| `class DirectTransport<TcpImpl, Framer, ...>` | **删除** | — |
| `class DirectTxTransport<TcpImpl, Framer, ...>` | **删除** | — |
| `class eph::dpdk::Reactor<>` | `class DpdkPoller<>` | eph-net-dpdk |
| (no UdpSocket) | `class KernelUdpSocket<C>` | eph-net-kernel |
| `class UdpSender` | `class DpdkUdpSocket<C>` | eph-net-dpdk |
| `class WsFramer` | `class WsCodec` | eph-codec |
| `class RawFramer` | `class RawStreamCodec` | eph-codec |
| `class LengthPrefixFramer` | `class LengthPrefixCodec` | eph-codec |
| (no MoldUDP64 codec) | `class Mold64Codec` | eph-codec |
| `class FakeTcpTransport` | `class FakeStream` | eph-net (test/) |
| (no FakeDatagram) | `class FakeDatagram` | eph-net (test/) |
| (no TestPoller) | `class TestPoller<P>` | eph-net (test/) |
| `enum class TcpState` | (保留) `enum class TcpState` | eph-net |
| (scattered ErrorEnum) | `enum class Error` + `struct ErrorInfo` | eph-core |
| `class ReconnectPolicy` | (保留) `class ReconnectPolicy` | eph-net |
| `eph-transport` | (删除) | — |
| `eph-dpdk` | `eph-net-dpdk` | — |
| `eph-net` (current) | `eph-net-kernel` + 新 `eph-net` (concepts) | — |
| `docs/reactor-guide.md` | `docs/poller-guide.md` | docs |

## 命名空间总表

```
eph::utils       (不变)
eph::containers  (不变)
eph::core        Codec concepts, ErrorInfo, Error enum, OutputBuffer
eph::codec       WsCodec, RawStreamCodec, LengthPrefixCodec, RawDatagramCodec, Mold64Codec
eph::net         Stream/Datagram/Pollable/Poller concepts, SocketAddr, ReconnectPolicy, TcpState
eph::net::test   FakeStream, FakeDatagram, TestPoller
eph::net::kernel KernelTcpStream, KernelUdpSocket, KernelPoller
eph::net::dpdk   DpdkTcpStream, DpdkUdpSocket, DpdkPoller, Eal, FlowSteering, ...
eph::fix         (不变) FixCodec satisfies Codec concept
eph::itch        (不变) ItchCodec, SoupBinTcpCodec
eph::json        (不变) JsonCodec
eph::book        (不变) order book bridges
```

---

## 状态：FROZEN

所有架构决议锁定。下一步：开始 Phase 0 实施。

实施过程中如发现架构层面的不可实现问题，需要重新走 /discuss 流程升级到 v3.4。否则按 Phase 0-8 顺序推进，每个 phase 一组独立 commit。
