# Plan: Direct TX/RX Transport 模式

> 新增编译期 TransportMode 参数，支持绕过 SPSC queue 的直接发送/接收路径。

创建时间：2026-04-02
状态：已完成

---

## 定位与边界

**目标**：在 Transport 模板中新增 kDirectTx 和 kDirect 两种运行模式，允许 app 线程直接执行 encode+encrypt+send 和 poll+decrypt+decode，消除 SPSC queue transit 延迟。

**In scope**：
- TlsRecordCrypto 拆分为 TlsEncryptor + TlsDecryptor
- TransportMode 枚举 + 编译期模板参数
- kDirectTx 模式：app 直接 TX，RX 线程正常
- kDirect 模式：app 直接 TX + RX，无后台线程
- SendError 新增错误码
- preset 别名扩展
- 单元测试

**Out of scope**：
- 自动重连（直接模式下不支持）
- 运行时模式切换
- 现有 kThreaded 模式行为变更

---

## 架构设计

### 运行模式

| 模式 | TX 路径 | RX 路径 | 线程 | Queue |
|------|---------|---------|------|-------|
| kThreaded | app → SPSC → TX 线程 → encode → encrypt → send | RX 线程 → poll → decrypt → decode → SPSC/callback → app | TX + RX | TX + RX |
| kDirectTx | app → encode → encrypt → send | RX 线程 → poll → decrypt → decode → SPSC/callback → app | RX only | RX only |
| kDirect | app → encode → encrypt → send | app → poll → decrypt → decode → callback | 无 | 无 |

### TLS 拆分

```
TlsRecordCrypto（兼容组合）
├── TlsEncryptor — enc_ctx_ + write_ki_ + write_seq_
└── TlsDecryptor — dec_ctx_ + read_ki_ + read_seq_
```

### 模板参数

```cpp
enum class TransportMode { kThreaded, kDirectTx, kDirect };

template <TcpTransport TcpImpl,
          MessageFramer Framer = WsFramer,
          TransportMode Mode = TransportMode::kThreaded,
          size_t MaxPayload = 512, size_t QueueDepth = 1024,
          template <typename, size_t> class RxQueueTmpl = BoundedQueue,
          bool LastOnlyDeliver = false>
class Transport;
```

### 成员变量编译期消除

```cpp
// TX queue/thread: 仅 kThreaded
[[no_unique_address]] std::conditional_t<Mode == kThreaded, TxQueue, detail::Empty> tx_queue_;
[[no_unique_address]] std::conditional_t<Mode == kThreaded, std::thread, detail::Empty> tx_thread_;

// RX queue/thread: kThreaded + kDirectTx
[[no_unique_address]] std::conditional_t<Mode != kDirect, RxQueue, detail::Empty> rx_queue_;
[[no_unique_address]] std::conditional_t<Mode != kDirect, std::thread, detail::Empty> rx_thread_;

// TLS: 拆分持有
std::unique_ptr<TlsEncryptor> encryptor_;   // TX owner (app or TX thread)
std::unique_ptr<TlsDecryptor> decryptor_;   // RX owner (app or RX thread)
```

---

## 接口设计

### send() — 统一签名，编译期分发

```cpp
[[nodiscard]] SendError send(const void* data, size_t len,
                             uint8_t opcode = ws::opcode::kBinary) noexcept {
    // 公共验证...
    if constexpr (Mode == TransportMode::kThreaded) {
        return enqueue_tx(data, len, opcode);
    } else {
        return send_direct(data, len, opcode);
    }
}
```

### send_direct() — 新增私有方法

```cpp
SendError send_direct(const void* data, size_t len, uint8_t opcode) noexcept {
    // 1. WS encode (stateless, thread-safe)
    // 2. TLS encrypt via encryptor_ (app 线程独占)
    // 3. TCP send via tcp_->send()
    // 返回 kEncryptFailed / kTcpSendFailed on failure
}
```

### poll() — 仅 kDirect 模式

```cpp
[[nodiscard]] std::expected<uint16_t, std::string> poll() noexcept
    requires (Mode == TransportMode::kDirect);
```

### SendError 扩展

```cpp
kEncryptFailed  = -7,  // TLS encrypt 失败（仅直接模式）
kTcpSendFailed  = -8,  // TCP send 失败（仅直接模式）
```

### Preset 别名

```cpp
template <typename T> using DirectTxTransport = Transport<T, WsFramer, kDirectTx, 512, 1024>;
template <typename T> using DirectTransport   = Transport<T, WsFramer, kDirect, 512, 1024>;
```

---

## 实施计划

### 阶段 1: TlsRecordCrypto 拆分
- 新建 tls_encryptor.hpp, tls_decryptor.hpp
- TlsRecordCrypto 改为组合 { TlsEncryptor enc; TlsDecryptor dec; }
- Transport 内部调用改为 crypto_->enc.encrypt() / crypto_->dec.decrypt()
- 验收：xmake build -g tests 零编译错误

### 阶段 2: TransportMode + kDirectTx
- 新增 TransportMode 枚举
- Transport 模板新增 Mode 参数
- send() if constexpr 分发 + send_direct() 实现
- create() 根据 Mode 条件启动线程
- 条件成员变量（std::conditional_t + [[no_unique_address]]）
- 新增 SendError::kEncryptFailed / kTcpSendFailed
- 验收：kThreaded 不变 + kDirectTx 编译通过

### 阶段 3: kDirect 模式
- poll() 实现（requires 约束）
- create() 不启动任何线程
- decryptor_ 由 app 线程持有
- 验收：kDirect 单线程收发可编译

### 阶段 4: 测试 + preset + 文档
- MockTcp 测试三种模式
- presets.hpp 新增 DirectTxTransport / DirectTransport
- eph-net / eph-dpdk 别名更新
- 验收：全量测试通过

---

## 关键决策记录

### D-1: 编译期模式选择
- **决策**：TransportMode 作为模板参数，if constexpr 消除无关代码
- **理由**：HFT 场景零额外分支开销

### D-2: TLS 拆分为独立 Encryptor/Decryptor
- **决策**：拆分为两个独立类，TlsRecordCrypto 保留为兼容组合
- **理由**：encrypt/decrypt 已使用独立 AEAD context + 序列号，拆分无需同步机制

### D-3: 直接模式不支持自动重连
- **决策**：send/poll 返回错误，app 自行重建 Transport
- **理由**：超低延迟场景不接受重连引入的不确定性
