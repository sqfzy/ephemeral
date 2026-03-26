# DPDK Multi-Symbol p99 Optimization Report

**日期**: 2026-03-26
**平台**: AWS EC2 c7g (Graviton3 arm64), ENA DPDK vfio-pci, 16 cores
**目标**: bench_market_multi_dpdk p99 稳定在几 us

---

## 基线数据

### 单 symbol DPDK (`bench_market_dpdk`, btcusdt@bookTicker)

| 指标 | 值 |
|---|---|
| **p50** | **444 ns** |
| **p99** | **1,980 ns (2.0 us)** |
| **p99.9** | 3,220 ns |
| **max** | 3,884 ns |
| decrypt p99 | 1.7 us |
| decode p99 | 0.3 us |

**结论: 单 symbol 已达标。p99 < 2us。**

### 多 symbol Combined (`bench_market_multi_dpdk`, 3 symbols)

| 模式 | p50 | p99 | p99.9 | decode p99 |
|---|---|---|---|---|
| all (无 filter) | 4.9 us | **62.5 us** | 78.0 us | 58.9 us |
| twophase (filter) | 2.0-3.8 us | **35-44 us** | 62-74 us | 33-41 us |

---

## 优化措施

### 1. make_twophase_filter: 消除双重 hash 计算

**问题**: Pass 1 和 Pass 2 各调用一次 `std::function` 提取 symbol hash，导致每帧两次间接调用。

**修复**: Cache hash 到栈上数组，Pass 2 直接复用。

**效果**: p99 从 41us → 35us（约 -15%），但市场波动导致难以精确量化。

### 2. process_ws_data_filtered: 合并 Phase 1-2 为单遍扫描

**问题**: Phase 1 扫描建 FrameIndexEntry → Phase 2 再遍历建 FrameView → Phase 3 再遍历分发。

**修复**: Phase 1 扫描时同步构建 FrameView，减少一轮遍历。

**效果**: 微小改善，主要节省 cache 压力。

---

## 根因分析

### 为什么 combined stream p99 > 30us

p99 瓶颈 = **batch 处理时间随帧数线性增长**。

Binance combined stream 将多个 symbol 的更新打包进单一 TLS record。在市场活跃时，单个 TLS record 可包含 30-100+ 帧。AES-GCM 要求整条 record 解密后才能处理，因此帧处理是串行的。

每帧成本:
- ws::decode_frame (头解析): ~200ns
- FrameView 构建: ~20ns
- dispatch + rx_enqueue (memcpy): ~700ns (仅投递帧)

30 帧 batch: 30 × 200ns + 3 × 700ns ≈ **8us** (理论最优)

实测 35-44us 包含:
- TLS 解密 per-record 开销
- Filter `std::function` 调用
- 实际帧可能更多（60+ 帧在峰值时段）
- cache miss 在大 buffer 上跳转

### 为什么单 symbol p99 = 2us

单 symbol 连接的 TLS record 仅包含 1-2 帧，无 batch 堆积。`LastOnlyDeliver=true` 跳过中间帧。

---

## 结论与架构建议

| 目标 | 方案 | p99 |
|---|---|---|
| 单 symbol 最低延迟 | `bench_market_dpdk` | **< 2us** |
| 多 symbol 最低延迟 | **每 symbol 独立连接** | **< 2us per symbol** |
| 多 symbol 单连接 | combined + twophase filter | ~35-44us |

### 推荐架构: 每 symbol 独立 Transport

```
                   ┌── Transport(btcusdt) ── TLS ── WS ── /ws/btcusdt@bookTicker
App ──┤            │
      ├── recv() ──┼── Transport(ethusdt) ── TLS ── WS ── /ws/ethusdt@bookTicker
      │            │
      └────────────┴── Transport(solusdt) ── TLS ── WS ── /ws/solusdt@bookTicker
```

每个 Transport 独立 TLS session + WS connection，TLS record 仅含单 symbol 帧。
代价: 3x TCP 连接、3x TLS session、3x RX/TX 线程（6 cores for 3 symbols）。

### DPDK 多连接的限制

尝试在 bench_market_multi_dpdk 中实现 `--split` 模式失败:
- DPDK Platform (mbuf pool) 是 singleton，第二次创建报 "File exists"
- 共享 Platform 的 `connect(platform&, ...)` API 存在，但 ENA 单 port 多 session 需要多 RX/TX 队列配置
- 当前 TcpSession 实现不支持同一 DPDK port 上的多 TCP 连接复用

**后续工作**: 扩展 DPDK Platform 支持多 queue pair，使 bench_market_multi_dpdk --split 可以在单进程内运行多个独立 Transport。
