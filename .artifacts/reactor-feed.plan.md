# Plan: Reactor + Transport 数据管道打通

> Transport 新增 feed_rx/process_pending，Reactor 新增 on_burst_complete，零耦合解决多连接 TLS/WS 管道断裂。

创建时间：2026-04-02
状态：已完成

---

## 定位与边界

**目标**：让 Reactor 分发的 TCP payload 能流入 Transport 的 TLS/WS 管道，同时保持两个模块零依赖。

**In scope**：
- Transport (eph-transport)：新增 `feed_rx()` + `process_pending()`，重构 `poll()`
- Reactor (eph-dpdk)：新增 `set_on_burst_complete()` 回调
- 更新测试和文档

**Out of scope**：
- Reactor 不持有 Transport（零耦合）
- 不新增 TransportMode（复用 kDirect）
- 不改 Reactor 的 NIC burst / 4-tuple 分发逻辑

---

## 架构设计

### 数据流

```
Reactor 线程（eph-dpdk，不知道 Transport）
│
├─ rte_eth_rx_burst()
├─ for each packet:
│   session->process_rx(pkt, on_data_callback)
│     └─ on_data_callback(tcp_payload, len)         ← 用户 app lambda
│         └─ tp->feed_rx(data, len)                 ← memcpy 到 reassembly buf
│
├─ on_burst_complete()                              ← 新增回调点
│   └─ 用户 app lambda:
│       └─ tp->process_pending()                    ← TLS decrypt → WS decode → on_message
│           └─ flush_pending_ack()
│
└─ 下一轮 burst
```

### 模块职责（不变）

| 模块 | 职责 | 知道对方？ |
|------|------|-----------|
| eph-dpdk (Reactor) | NIC burst + 4-tuple 分发 + 回调 | 不知道 Transport |
| eph-transport (Transport) | TLS/WS 管道 + 连接生命周期 | 不知道 Reactor |
| 用户 app | 在回调里把两者接起来 | 知道两者 |

---

## 接口设计

### Transport 新增（eph-transport/transport.hpp）

```cpp
/// 将原始 TCP payload 积累到 reassembly buffer。
/// 仅 memcpy，不触发 decrypt/decode。
/// 单线程调用（Reactor 线程或 app 线程）。
void feed_rx(const uint8_t* data, uint16_t len) noexcept
    requires (!kHasRxThread);

/// 处理 reassembly buffer 中所有积累的数据。
/// TLS decrypt → WS decode → on_message → flush_pending_ack。
/// 单线程调用，与 feed_rx 同一线程。
void process_pending() noexcept
    requires (!kHasRxThread);
```

**poll() 重构**（kDirect 模式，行为不变）：
```cpp
auto poll() noexcept requires (kIsDirect) {
    auto result = tcp_->poll_rx([this](const uint8_t* d, uint16_t l) {
        feed_rx(d, l);
    });
    process_pending();
    return result;
}
```

**feed_rx 内部**：
```cpp
void feed_rx(const uint8_t* data, uint16_t len) noexcept {
    auto& rx = direct_rx_;
    if (config_.use_tls) {
        // 积累到 TLS reassembly buffer
        if (rx.reassembly_len + len <= kReassemblyBufSize) {
            std::memcpy(rx.reassembly_storage.get() + rx.reassembly_len, data, len);
            rx.reassembly_len += len;
        }
    } else {
        // 积累到 WS reassembly buffer
        if (rx.ws_reassembly_len + len <= kWsReassemblyBufSize) {
            std::memcpy(rx.ws_reassembly_storage.get() + rx.ws_reassembly_len, data, len);
            rx.ws_reassembly_len += len;
        }
    }
}
```

**process_pending 内部**：现有 poll() 中 `tcp_->poll_rx()` 回调之后的全部逻辑——TLS record decrypt loop + WS frame processing + flush_pending_ack。

### Reactor 新增（eph-dpdk/reactor.hpp）

```cpp
using BurstCompleteCallback = std::function<void()>;

/// 注册 per-burst 完成回调。每次 rte_eth_rx_burst 分发完所有包后调用。
/// burst 取到 0 个包时不调用。
void set_on_burst_complete(BurstCompleteCallback cb);
```

在 Reactor::start() 的 rx_loop 中，每次 burst + dispatch 完成后：
```cpp
if (nb_rx > 0 && on_burst_complete_) {
    on_burst_complete_();
}
```

---

## 实施计划

### 阶段 1: Transport feed_rx + process_pending

- 从现有 poll() 中提取 feed_rx()（memcpy 积累部分）
- 从现有 poll() 中提取 process_pending()（decrypt + decode + flush_ack 部分）
- 重构 poll() 为 feed_rx + process_pending 的组合
- requires (!kHasRxThread) 约束
- 验收：poll() 行为不变（现有测试通过），feed_rx + process_pending 可独立调用

### 阶段 2: Reactor on_burst_complete

- 新增 BurstCompleteCallback 成员 + set_on_burst_complete()
- 在 rx_loop 的 burst 分发后调用
- 验收：现有 Reactor 测试通过，新回调被调用

### 阶段 3: 集成测试 + 文档

- 编写 Reactor + Transport(kDirect) 集成示例
- 更新 reactor-guide.md
- 验收：示例可编译

---

## 关键决策记录

### D-1: Reactor 和 Transport 零耦合
- **问题**：Reactor 应该持有 Transport 还是通过回调间接对接？
- **决策**：回调间接对接，零依赖
- **理由**：Reactor 和 Transport 负责不同场景（多路复用 vs 协议栈），不应强制绑定。用户在 app 层组装。

### D-2: feed_rx 和 process_pending 分离
- **问题**：feed_rx 是否应该内含 process（decrypt+decode）？
- **决策**：分离为两步
- **理由**：Reactor 一次 burst 分发给多个连接，先全部 feed_rx（纯 memcpy），再逐连接 process_pending（重计算），利于 CPU cache。
