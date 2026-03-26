# Discussion Record — Pipeline Instrumentation Points

## Context
- 时间：2026-03-25 18:18:13
- 用户需求：梳理当前管线打点位置，识别可增加的打点以深入了解延迟分布

## 当前管线与打点位置

### TX 路径（App → 网线）

```
App thread                           TX thread                        Kernel/NIC
─────────────                        ─────────                        ──────────
send_ping()                          drain SPSC queue
  │                                    │
  ├─[enqueue_tsc]──→ SPSC Queue ──→   ├─ WS frame encode
  │                                    ├─ TLS encrypt
  │                                    ├─[flush_tsc]
  │                                    ├─ tcp_->send()
  │                                    │                               ├─ kernel TX stack
  │                                    │                               ├─[kernel_tx_ts]
  │                                    │                               └─ wire departure
```

**已有打点**：`enqueue_tsc`, `flush_tsc`, `kernel_tx_ts`
**已有 histogram**：`tx_latency = flush - enqueue + kernel_tx_delay`

**可加的打点**：

| 打点 | 位置 | 拆分的段 | 价值 |
|------|------|---------|------|
| `drain_tsc` | TX loop 取出消息后、编码前 | queue wait = drain - enqueue | 高：区分 queue 排队 vs 编码加密开销 |
| `encode_done_tsc` | WS encode 后、TLS encrypt 前 | encode = encode_done - drain | 中：区分 WS 编码 vs TLS 加密 |

### RX 路径（网线 → App）

```
Kernel/NIC                           RX thread                             App thread
──────────                           ─────────                             ──────────
NIC → kernel RX stack                poll_rx() / recvmsg()
  │                                    │
  ├─[kernel_rx_ts]                     ├─[arrival_tsc]
  │                                    ├─ TLS reassembly
  │                                    ├─ TLS decrypt
  │                                    ├─ WS frame decode
  │                                    ├─[record_rx_latency]
  │                                    ├─ deliver_message
  │                                    │   ├─ on_message callback (push)
  │                                    │   └─ rx_enqueue (pull) ──→ SPSC Queue ──→ recv()
```

**已有打点**：`kernel_rx_ts`, `arrival_tsc`, `record_rx_latency`(decode 完成)
**已有 histogram**：`rx_latency = decode - arrival + kernel_rx_delay`

**可加的打点**：

| 打点 | 位置 | 拆分的段 | 价值 |
|------|------|---------|------|
| `decrypt_done_tsc` | TLS decrypt 完成后、WS decode 前 | decrypt = decrypt_done - arrival | 高：区分 TLS 开销 vs WS 解帧开销 |
| `deliver_tsc` | deliver_message 入口 | WS decode = deliver - decrypt_done | 中：WS 解帧本身开销 |
| `app_recv_tsc` | recv() 返回（pull 模式）| queue wait = app_recv - deliver | 高：RX queue 排队延迟（拉模式） |

## 推荐优先级

**高价值（建议加）**：
1. **TX drain_tsc** — 区分 "消息在 SPSC queue 里等了多久" vs "编码加密花了多久"
2. **RX decrypt_done_tsc** — 区分 "TLS 解密花了多久" vs "WS 解帧花了多久"

**中等价值（可选）**：
3. TX encode_done_tsc — 进一步拆分 WS 编码 vs TLS 加密
4. RX app_recv_tsc — pull 模式的 RX queue 排队延迟（但这在 Transport 外部，需要用户自己测）

**低价值（不建议）**：
- 每个 TLS record 的逐条解密打点 — 粒度太细，开销大于收益
