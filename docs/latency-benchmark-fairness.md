# Latency Benchmark Fairness Audit

Socket (kernel) vs DPDK (kernel-bypass) 延迟对比方案的公平性审计报告。

## 背景

Transport 库提供两个后端：
- **SocketTransport**: POSIX socket，数据经过内核协议栈
- **DPDK TcpSession**: 内核旁路，用户态 TCP 直接操作 NIC

两个后端共用同一个 `Transport<>` 模板，延迟打点代码完全相同。
核心挑战：Socket 版的内核协议栈延迟（NIC→recv, send→wire）在 Transport 层不可见，
需要通过 `SO_TIMESTAMPING` 补齐才能实现公平对比。

## 四个延迟指标

| # | 指标 | 物理含义 | 起点 | 终点 |
|---|------|---------|------|------|
| 1 | TX Queue | ping(模拟订单)产生 → tx_burst 发出 | `send_ping()` 入队时 `msg.tsc = TSC::now()` | TX loop 编码加密后 `flush_tsc = TSC::now()` |
| 2 | RX Pong Pipeline | rx_burst 收到数据 → pong(模拟响应)解码完成 | `poll_rx()` 返回后 `current_arrival_tsc_ = TSC::now()` | WS frame decode 后 `record_rx_latency()` |
| 3 | RX Data Pipeline | rx_burst 收到数据 → 行情消息解码完成 | 同上 | 同上（同一个 `record_rx_latency()` 入口） |
| 4 | RTT | ping tx_burst → pong rx_burst 端到端 | TX loop 发送 ping 前 `last_ping_tsc_ = TSC::now()` | pong 解码时 `pong_tsc = TSC::now()` |

## 公平性机制

### EnableTimestamps 编译期开关

```cpp
// Transport 层：消息结构自带 TSC 字段，内部 histogram 记录延迟
template <..., bool EnableTimestamps = false, ...>
class Transport;

// SocketTransport 层：SO_TIMESTAMPING 获取内核栈延迟
template <bool EnableTimestamps = false>
class SocketTransport;

// 一个 const 统一控制
constexpr bool kTimestamps = true;
using HftTransport = Transport<SocketTransport<kTimestamps>, WsFramer, 512, 1024, kTimestamps, EvictingQueue>;
```

`EnableTimestamps = false` 时全路径 `if constexpr` 消除，零开销。

### Socket 内核栈延迟补齐

`SocketTransport<true>` 在 `connect()` 时设置 `SO_TIMESTAMPING`：
- `SOF_TIMESTAMPING_RX_SOFTWARE`：内核在 `netif_receive_skb()` 打时间戳（NIC 驱动交付包到内核）
- `SOF_TIMESTAMPING_TX_SOFTWARE`：内核在 packet 实际离开时打时间戳
- `SOF_TIMESTAMPING_OPT_TSONLY`：不拷贝原始包到 error queue，减少开销

`poll_rx()` 使用 `recvmsg()` + cmsg 提取 RX 内核时间戳，顺带 poll error queue 获取 TX 内核时间戳。

Transport 通过 `if constexpr (requires { tcp_->last_kernel_rx_delay_ns(); })` 读取内核栈延迟，
自动加到 pipeline 延迟上：

```
Socket stats().rx_latency = kernel_rx_delay + pipeline_delay  (全路径)
DPDK   stats().rx_latency = pipeline_delay                    (本身就是全路径)
```

### 延迟记录路径

每个 histogram 全局唯一记录点：

```
tx_latency_histogram_.record()   — TX loop per-message，编码加密后
rx_latency_histogram_.record()   — record_rx_latency() helper，每个 decoded frame
rtt_histogram_.record()          — pong 处理分支
```

`record_rx_latency()` 是所有帧类型（data/pong/ping/close）的统一入口，
在 `process_ws_data` 和 `process_generic_data` 的 decode loop 中调用。

## 逐指标公平性验证

### 指标 1: TX Queue (ping → tx_burst)

| 环节 | Socket | DPDK |
|------|--------|------|
| enqueue_tsc | `msg.tsc = TSC::now()` in `send_ping()` | 同一代码 |
| flush_tsc | `TSC::now()` in TX loop | 同一代码 |
| kernel TX delay | `+ tcp_->last_kernel_tx_delay_ns()` (send→wire) | `requires` = false → +0 |
| **total** | **pipeline + kernel_tx** | **pipeline** |

- per-message: tsc 嵌入 TxMessage，每条消息独立 ✅
- 公平: Socket 补齐 kernel TX delay ✅

### 指标 2 & 3: RX Pipeline (rx_burst → pong/data)

| 环节 | Socket | DPDK |
|------|--------|------|
| arrival_tsc | `TSC::now()` after `poll_rx()` returns | 同一代码 |
| decode_tsc | `TSC::now()` in `record_rx_latency()` | 同一代码 |
| kernel RX delay | `+ tcp_->last_kernel_rx_delay_ns()` (NIC→recv) | `requires` = false → +0 |
| **total** | **kernel_rx + pipeline** | **pipeline** |

- per-message: 每个 decoded frame 调一次 `record_rx_latency()` ✅
- 所有帧类型（data/pong/ping/close）走同一路径 ✅
- 公平: Socket 补齐 kernel RX delay ✅

### 指标 4: RTT (ping tx_burst → pong decode)

| 环节 | Socket | DPDK |
|------|--------|------|
| ping_tsc | `TSC::now()` before `tcp_->send()` in TX loop | 同一代码 |
| pong_tsc | `TSC::now()` at pong decode | 同一代码 |
| kernel delays | `+ kernel_tx_delay + kernel_rx_delay` | `requires` = false → +0+0 |
| **total** | **rtt + kernel_tx + kernel_rx** | **rtt** |

- per-message: `last_ping_tsc_` atomic，顺序 ping 模式下 1:1 配对 ✅
- 公平: Socket 补齐双向 kernel delay ✅

## 共享打点检查

| 资源 | 写入方 | 读取方 | 共享？ |
|------|--------|--------|--------|
| `TxMessage.tsc` | App 线程 per-message | TX 线程 per-message | ❌ 嵌入消息，不共享 |
| `current_arrival_tsc_` | RX 线程 per-poll_rx | RX 线程 `record_rx_latency()` | 同一 poll_rx 的多 frame 共享（物理事实：同一次 recv/rx_burst 到达） |
| `last_ping_tsc_` | TX 线程 | RX 线程 | atomic，顺序 ping 下 1:1 配对 |

`current_arrival_tsc_` 在同一 poll_rx 的多 frame 间共享是物理事实，非设计缺陷——
这些数据确实在同一次 `recv()`/`rte_eth_rx_burst()` 中到达。两个后端行为完全一致。

## 时钟域

| 度量 | 时钟域 | 闭环？ |
|------|--------|--------|
| TX/RX pipeline delay | TSC (rdtsc) | ✅ 两端都是 TSC::now() |
| Kernel RX/TX delay | CLOCK_REALTIME | ✅ 两端都是 clock_gettime(CLOCK_REALTIME) |
| 合并 | pipeline_ns (from TSC) + kernel_ns (from CLOCK_REALTIME) | ⚠️ 跨域相加，两者都已转为纳秒 |

跨域相加的误差来源：TSC→ns 转换精度（取决于 TSC 校准质量，通常 <1ns 误差）。
对于 μs 级的延迟测量，此误差可忽略。

## 结论

**四指标全部满足要求：**
- ✅ 绝对公平（Socket 通过 SO_TIMESTAMPING 补齐内核栈延迟）
- ✅ Per-message（每条消息独立打点）
- ✅ 无设计级共享（物理共享除外）
- ✅ 零开销路径（EnableTimestamps=false 时 if constexpr 消除一切）
