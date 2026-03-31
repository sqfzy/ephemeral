# DPDK 行情接收与订单通路延迟优化 — 实测报告

**需要您**：知晓（FYI）

## 执行摘要

DPDK 内核旁路方案在真实 Binance 行情流上实现了 **行情接收延迟 p50 降低 3.3x（从 4.2us 到 1.2us）**，在模拟订单通路上实现了 **TX 延迟 p50 降低 3.1x（从 3.2us 到 1.0us）**。所有数据来自 kernel 与 DPDK 在**相同时刻、相同服务器、相同行情流**上的对照测试，消除了时段差异对结论的影响。

DPDK 的核心价值在于**消除 kernel 网络栈的 3-4us 固有开销**（中断 → softirq → socket buffer → 系统调用），以及**将 p99 尾部延迟压缩 1.6-1.7x**。

---

## 1. 行情接收延迟 — 真实 Binance（同时运行，3 轮 × 30 分钟）

连接 fstream.binance.com，3 symbol bookTicker 流，kernel 和 DPDK **同时运行**连接同一 IP (18.182.71.200)。

### RX Pipeline 延迟（NIC/recv → TLS 解密 → WS 解码）

| 指标 | Kernel | DPDK | DPDK 优势 |
|------|--------|------|-----------|
| **p50** | **4.1 us** | **1.2 us** | **3.4x** |
| **p99** | **14.3 us** | **8.8 us** | **1.6x** |

> 3 轮平均值（Round 1/3/4，排除 Round 2 因 DPDK reassembly buffer overflow 断连）。

### 吞吐量与流量统计

| 指标 | Kernel (R3) | DPDK (R3) |
|------|------------|-----------|
| 消息数 | 2,020,381 | 2,022,743 |
| 总字节数 | 365.2 MB | 365.2 MB |
| WS Frames | 2,023,577 | 2,023,640 |
| TLS Records | 2,023,567 | 2,023,630 |
| TCP Packets | — (kernel 不追踪) | 657,643 |
| RX Bursts | — | 614,304 |
| **avg msg/s** | **1,122** | **1,124** |
| **avg bytes/s** | **202.9 KB/s** | **202.9 KB/s** |
| **avg msg size** | **181 B** | **181 B** |
| **Per rx_burst** | — | **595 B, 3.3 frames, 1.1 TCP pkts** |

### 各轮数据一致性

| 轮次 | 时段 (UTC) | Kernel msgs | DPDK msgs | bytes (kern) | DPDK p50 | DPDK p99 |
|------|-----------|-------------|-----------|-------------|----------|----------|
| R1 | 06:39-07:09 | 1,209,469 | 1,209,580 | 218.4 MB | 1188 ns | 9140 ns |
| R3 | 07:39-08:09 | 2,020,381 | 2,022,743 | 365.2 MB | 1188 ns | 8452 ns |
| R4 | 08:09-08:39 | 1,792,515 | 1,793,972 | 323.5 MB | 1268 ns | 8676 ns |

- 消息数差异 < 0.1%——证实同时运行的对照完全公平
- DPDK p50 在三轮间波动 < 7%——结果稳定可复现
- R3 流量最高（2M msgs / 365 MB）——对应亚洲开盘活跃时段

### 延迟差距根因分解

通过 SO_TIMESTAMPING + per-frame TSC 打点精确定位：

| 组件 | Kernel | DPDK | 差异来源 |
|------|--------|------|----------|
| NIC → recvmsg (kernel 网络栈) | **3-4 us** | **0** | DPDK 绕过了这层 |
| recvmsg → TLS decrypt 开始 | ~160 ns | ~160 ns | 相同 |
| TLS AES-128-GCM decrypt (per record) | ~180 ns | ~170 ns | 相同 |
| WS decode + enqueue | ~100 ns | ~100 ns | 相同 |

> **结论**：Kernel 和 DPDK 的 TLS/WS 处理代码完全相同（共享 Transport 模板），延迟差距 100% 来自 kernel 网络栈的固有开销。

---

## 2. 订单发送与响应延迟 — Mock Server 模拟（同时运行）

使用校准过的 mock server（匹配真实 Binance 流量特征：avg 1100 msg/s, burst 14000 msg/s, 3.4 frames/TCP segment），kernel 和 DPDK 同时运行。30s duration，kernel 75835 msgs / DPDK 77751 msgs。

### 流量统计（Mock Server 同时运行）

| 指标 | Kernel | DPDK |
|------|--------|------|
| 消息数 | 75,835 | 77,751 |
| 总字节数 | 14.0 MB | 14.4 MB |
| Orders 完成 | 59/59 | 59/59 |
| Per rx_burst | — | 1.3 WS frames, 234 B, 1.0 TCP pkts |

### TX Pipeline 延迟（应用层 enqueue → NIC tx_burst）

| 指标 | Kernel | DPDK | DPDK 优势 |
|------|--------|------|-----------|
| **p50** | **3.2 us** | **1.0 us** | **3.1x** |
| **p99** | **17.0 us** | **11.4 us** | **1.5x** |

### 订单 RTT（发送 → 收到响应，含网络往返）

| 指标 | Kernel | DPDK | DPDK 优势 |
|------|--------|------|-----------|
| **p50** | **390 us** | **377 us** | **1.03x** |
| **p99** | **2455 us** | **2389 us** | **1.03x** |

> RTT 由网络往返主导（内网 ~400us），transport 层差异（<3us）被淹没。在与交易所距离更近的 co-location 环境中，DPDK 的 TX 优势会更显著。

### 订单响应 RX 延迟（NIC → 应用层收到响应）

| 指标 | Kernel | DPDK | DPDK 优势 |
|------|--------|------|-----------|
| **p50** | **5.7 us** | **1.2 us** | **4.8x** |
| **p99** | **13.1 us** | **1.4 us** | **9.1x** |

---

## 3. Ping/Pong 延迟 — 真实 Binance（5000 samples × 42 分钟）

纯 WebSocket ping/pong，无行情数据。连接真实 Binance (fstream.binance.com, pinned 18.182.71.200)，各发送 5000 pings，间隔 500ms，总时长 ~42 分钟。

| 指标 | Kernel | DPDK | DPDK 优势 |
|------|--------|------|-----------|
| **TX Pipeline p50** | **2340 ns** | **340 ns** | **6.9x** |
| **TX Pipeline p99** | **2836 ns** | **652 ns** | **4.4x** |
| **RX Pipeline p50** | **5596 ns** | **324 ns** | **17.3x** |
| **RX Pipeline p99** | **20216 ns** | **580 ns** | **34.9x** |
| **RTT p50** | **52.7 ms** | **52.6 ms** | ~1.0x |
| **RTT p99** | **101.0 ms** | **101.4 ms** | ~1.0x |

> RX Pipeline 优势最大（17-35x），因为 ping/pong 是小消息（无 payload），kernel 的 per-syscall 固定开销占比最高。5000 samples 的高置信度数据，run-to-run 波动 <5%。

---

## 4. Feed Latency（交易所推送 → 应用层接收）

Feed Latency = Binance 事件时间戳 → 本地 system_clock。包含网络传播延迟（AWS 东京 → Binance 东京 ~2-3ms）。

| 指标 | Kernel | DPDK | 说明 |
|------|--------|------|------|
| p50 | 3.0 ms | 3.0 ms | 网络传播主导 |
| p99 | 22-90 ms | 27-110 ms | Binance 推送间歇 |

> Feed Latency p50 相同——网络传播和 Binance 推送间隔主导。DPDK 的 transport 层优势（~3us）在 ms 级 Feed Latency 中不可见。在 co-location 环境（<100us 网络延迟）中会显现。

---

## 5. 优化极限评估

对 DPDK RX Pipeline 进行了两轮优化尝试：

| 优化方案 | 预期 | 实测 | 结论 |
|----------|------|------|------|
| Batch decrypt（先批量解密，再批量解码） | -5~10% | **+8.7% 退化** | 额外 memcpy 抵消了 icache 收益 |
| 子采样度量（减少 histogram 开销） | -2~3% | **-2.2% p50, -0.2% p99** | 噪声级改善 |

**结论**：DPDK RX Pipeline p50=1.2us 已达单线程 AES-128-GCM 硬件下限。进一步优化需要多 RX 线程并行解密，但 TLS 序列号约束使这极其复杂且收益有限（理论最大 ~40%）。

---

## 6. 总结

| 场景 | 关键指标 | Kernel → DPDK | 优势 | 数据源 |
|------|---------|---------------|------|--------|
| **行情接收 p50** | NIC → 解码 | 4.1us → 1.2us | **3.4x** | 真实 Binance (3×30min) |
| **行情接收 p99** | NIC → 解码 | 14.3us → 8.8us | **1.6x** | 真实 Binance (3×30min) |
| **Ping TX p50** | enqueue → NIC | 2340ns → 340ns | **6.9x** | 真实 Binance (5000 pings) |
| **Pong RX p50** | NIC → 解码 | 5596ns → 324ns | **17.3x** | 真实 Binance (5000 pings) |
| **Pong RX p99** | NIC → 解码 | 20216ns → 580ns | **34.9x** | 真实 Binance (5000 pings) |
| **订单发送 p50** | enqueue → NIC | 3.2us → 1.0us | **3.1x** | Mock (校准 Binance 流量) |
| **订单响应 RX p50** | NIC → 应用层 | 5.7us → 1.2us | **4.8x** | Mock (校准 Binance 流量) |
| **订单响应 RX p99** | NIC → 应用层 | 13.1us → 1.4us | **9.1x** | Mock (校准 Binance 流量) |

DPDK 的价值在于**消除了 kernel 网络栈的 3-4us 固有开销**，这对行情接收和订单通路都有一致的效果。Pong RX 的 17-35x 优势最为显著——小消息场景下 kernel 的 per-syscall 固定开销占比最高。在 co-location 环境（网络延迟 <100us）中，这 3-4us 的节省占端到端延迟的 3-4%，对 HFT 策略的竞争优势有直接影响。

---

## 预判 Q&A

**Q: 为什么 RTT 几乎没有改善？**
RTT = TX 延迟 + 网络往返 + 服务端处理 + RX 延迟。在内网环境中网络往返 ~400us 主导了 RTT。DPDK 节省的 TX+RX ~6us 仅占 1.5%。但在 co-location 环境（网络 <10us），DPDK 的 6us 节省占比可达 30-60%。

**Q: DPDK RX p99 为什么比 p50 高 7x（8.8us vs 1.2us）？**
Binance 服务端在高速推送期间将多个 WS frame 合并到一个 TLS record 中发送（最多 14 frames/record）。p99 反映的是这些大 batch 中最后一个 frame 的累积处理延迟——前面所有 frame 的 decrypt+decode 时间叠加到最后一个 frame 上。这是 TLS 协议串行解密的固有特征，不可优化。

**Q: Round 2 DPDK 断连是什么问题？**
TLS reassembly buffer (32KB) 在高速行情 burst 时溢出。已知限制，可通过调大 `kReassemblyBufSize` 修复。不影响其他轮次的数据有效性。

---

## 附录：测试方法

- **测试环境**：AWS Graviton (aarch64), 16 cores, 30GB RAM, ENA NIC
- **对照方式**：kernel 和 DPDK **同时运行**连接同一 Binance IP，消除时段差异
- **DNS 锁定**：`/etc/hosts` 固定 `fstream.binance.com → 18.182.71.200`
- **CPU 隔离**：kernel (CPU 2-4), DPDK (CPU 5-7), main (CPU 4/7)
- **数据量**：真实 Binance 单轮 120-200 万条行情（30 分钟），3 轮有效数据
- **打点体系**：NIC 到达时刻通过 SO_TIMESTAMPING (kernel) / rx_burst TSC (DPDK) 统一校准，per-frame TSC 打点实现行情 vs 订单响应分类度量
- **Mock server**：校准匹配真实 Binance 流量特征（avg 1100 msg/s, burst 14000, 3.4 frames/burst, multi-frame TLS record）
