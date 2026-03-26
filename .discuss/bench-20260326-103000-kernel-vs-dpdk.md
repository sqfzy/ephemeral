# Kernel vs DPDK Multi-Symbol Benchmark Report

**日期**: 2026-03-26
**平台**: AWS EC2 c7g (Graviton3 arm64), 16 cores, Amazon Linux 2023
**NIC**: ENA (Elastic Network Adapter), Kernel: ens34 (27:00.0), DPDK: vfio-pci (28:00.0)
**目标**: `bench_market_multi` (Socket) vs `bench_market_multi_dpdk` (DPDK kernel-bypass)
**数据源**: Binance fstream combined bookTicker (btcusdt, ethusdt, solusdt)
**模式**: twophase (on_frame_filter, latest-per-symbol dedup)
**每轮时长**: 30s，共 5 轮同步运行（pinned 到同一 server IP）
**Server IP**: 52.195.241.154 (ap-northeast-1)

---

## 1. 测试配置

| 参数 | Kernel (Socket) | DPDK |
|---|---|---|
| 后端 | `SocketTransport` | `TcpSession<>` (kernel bypass) |
| MaxPayload | 16384 | 16384 |
| QueueDepth | 1024 | 1024 |
| LastOnlyDeliver | false | false |
| Frame Filter | twophase | twophase |
| CPU Affinity | 无 | EAL: 4-7, RX: 8, TX: 9, main: 10 |
| TLS | TLSv1.3, AES-128-GCM-SHA256 | 同左 |
| 去重率 | ~75% | ~75% |

两个 benchmark 同时启动，通过 `/etc/hosts` 固定到同一 Binance server IP，确保接收相同市场数据。

---

## 2. RX Pipeline 延迟

### 2.1 总体对比（R1，唯一完整同步轮次）

| 百分位 | Kernel | DPDK | DPDK 改善 |
|---|---|---|---|
| **min** | 4,200 ns | **1,500 ns** | **-64%** |
| **p50** | 6,188 ns | **1,908 ns** | **-69%** |
| **p99** | 50,576 ns | **41,840 ns** | **-17%** |
| **p99.9** | 114,784 ns | **64,496 ns** | **-44%** |
| **max** | 148,808 ns | **73,885 ns** | **-50%** |
| **mean** | 9,600 ns | **4,600 ns** | **-52%** |

### 2.2 延迟拆解（R1）

| 阶段 | Kernel (p50 / p99) | DPDK (p50 / p99) |
|---|---|---|
| **decrypt** | 0.6 / 9.8 us | 0.4 / 2.7 us |
| **decode** | 1.6 / 40.0 us | 1.5 / 39.6 us |
| **total RX** | 6.2 / 50.6 us | **1.9 / 41.8 us** |

- decrypt: DPDK 快，因为数据从 NIC 直达用户态，无 kernel buffer 拷贝
- decode: 两者一致（纯计算，与网络后端无关）
- total 差异 = 收包路径差异（kernel syscall + socket buffer vs DPDK poll-mode）

### 2.3 分桶对比：Traffic vs Latency（R1）

| 流量段 | | msg/s 范围 | p50 (ns) | p99 (ns) | p99.9 (ns) |
|---|---|---|---|---|---|
| **Low** | Kernel | 74–169 | 5,841 | 17,516 | 19,112 |
| | DPDK | 76–156 | **1,829** | **4,777** | **5,183** |
| **Mid** | Kernel | 200–472 | 6,399 | 36,554 | 46,882 |
| | DPDK | 292–677 | **1,855** | **35,208** | **46,639** |
| **High** | Kernel | 560–3577 | 7,075 | 58,800 | 78,993 |
| | DPDK | 743–4878 | **2,530** | **42,549** | **60,523** |

#### 按流量段的 DPDK 加速比

| 流量段 | p50 加速 | p99 加速 | 说明 |
|---|---|---|---|
| Low (<300 msg/s) | **3.2x** | **3.7x** | 小 batch，DPDK 收包优势全部体现 |
| Mid (300-700 msg/s) | **3.4x** | **1.0x** | p99 趋近，batch 处理开始主导 |
| High (>700 msg/s) | **2.8x** | **1.4x** | p50 仍快，p99 差距缩小（decode 主导） |

---

## 3. Kernel 5 轮稳定性

DPDK 因 EAL 资源释放问题仅 R1 有效，Kernel 5 轮全部有效。

### 3.1 RX Pipeline 延迟

| 轮次 | samples | p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |
|---|---|---|---|---|---|
| R1 | 5,600 | 6,188 | 50,576 | 114,784 | 148,808 |
| R2 | 5,403 | 5,700 | 50,576 | 79,456 | 101,137 |
| R3 | 5,259 | 5,668 | 41,936 | 71,136 | 91,769 |
| R4 | 6,143 | 5,532 | 36,752 | 68,832 | 78,743 |
| R5 | 6,863 | 4,836 | 46,320 | 76,832 | 87,321 |
| **中位数** | **5,600** | **5,668** | **46,320** | **76,832** | **91,769** |
| **范围** | | 4,836–6,188 | 36,752–50,576 | 68,832–114,784 | 78,743–148,808 |

### 3.2 分桶趋势（Kernel 5 轮）

| 轮次 | Low p99 | Mid p99 | High p99 |
|---|---|---|---|
| R1 | 17,516 | 36,554 | 58,800 |
| R2 | 13,282 | 31,927 | 58,647 |
| R3 | 10,987 | 20,979 | 41,983 |
| R4 | 11,886 | 27,642 | 41,221 |
| R5 | 10,508 | 38,857 | 48,951 |
| **中位数** | **11,886** | **31,927** | **48,951** |

### 3.3 Feed Latency（Kernel 5 轮）

| 轮次 | samples | p50 | p99 | max |
|---|---|---|---|---|
| R1 | 22,552 | 2.0 ms | 17.0 ms | 19.0 ms |
| R2 | 21,159 | 2.0 ms | 3.0 ms | 5.0 ms |
| R3 | 15,385 | 2.0 ms | 56.0 ms | 59.0 ms |
| R4 | 16,891 | 2.0 ms | 72.0 ms | 77.0 ms |
| R5 | 29,286 | 2.0 ms | 26.0 ms | 29.0 ms |

DPDK R1: p50=2.0ms, p99=16.0ms, max=17.0ms

- p50 恒定 2.0ms（网络 RTT ~1.8ms + NTP 粒度 1ms）
- p99 波动大（3–72ms）：受 Binance 服务端延迟突发和网络抖动影响，非本地处理差异
- Kernel 与 DPDK 的 Feed Latency 在 NTP 精度（~1ms）内无法区分

---

## 4. 分析

### 4.1 DPDK 的核心优势

DPDK 在 **p50（中位数延迟）** 上稳定提供 **2.6-3.4x** 加速：

```
Kernel 收包路径:  NIC → kernel driver → socket buffer → syscall → userspace copy → decrypt
DPDK 收包路径:    NIC → userspace poll (rx_burst) → decrypt
                                     ↑
                          省去: interrupt + context switch + kernel buffer + syscall + copy
                          节省: ~3-4 us
```

这 ~3-4us 的节省恰好与 p50 差距（6.2us - 1.9us = 4.3us）吻合。

### 4.2 为什么 p99 差距小

p99 反映的是 **高流量突发时段**（batch 内 30-100+ 帧串行处理）。此时：
- 帧扫描 30 帧 × ~200ns = 6us
- Filter 调用 + 投递 3-6 帧 × ~700ns = 2-4us
- TLS 解密整条 record = 2-5us
- **总计 ~10-15us 是 decode 路径的固定成本**

收包路径省去的 3-4us 在 40-50us 的总延迟中占比仅 ~8%，所以 p99 差距小。

### 4.3 Feed Latency 无差异的原因

```
Feed Latency = Binance 内部处理 + 网络传播 + NIC 收包 + RX pipeline
             ≈ ~0.5ms           + ~0.9ms    + <5us     + <50us
             ≈ 1.4ms (理论值)

NTP 精度 ≈ 1ms → 测量粒度 = 1ms
DPDK 节省 ≈ 4us → 0.004ms，在 1ms 粒度下不可见
```

Feed Latency 适合监控 **网络路径健康度**，不适合量化 DPDK vs Kernel 的微秒级差异。

---

## 5. 结论

| 维度 | Kernel | DPDK | 推荐 |
|---|---|---|---|
| **p50 延迟** | 5.7 us | **1.9 us** | DPDK (3x faster) |
| **p99 延迟** | 46 us | **42 us** | DPDK (marginal) |
| **Feed Latency** | 2.0 ms | 2.0 ms | 无差异 |
| **部署复杂度** | 低（标准 socket） | 高（hugepages, vfio, 独立 NIC） | Kernel |
| **CPU 占用** | 低（event-driven） | 高（poll-mode, 独占 2-3 cores） | Kernel |
| **多连接** | 简单（fork/thread） | 复杂（共享 Platform, 多队列） | Kernel |

### 选择建议

- **DPDK**：追求最低 p50（亚微秒级竞争优势），能承受专用 NIC + cores 的成本
- **Kernel**：通用场景、多 symbol 多连接、部署简单优先
- **两者共同瓶颈**：combined stream 的 batch 处理延迟（p99 ~40us），需拆为独立连接才能突破
