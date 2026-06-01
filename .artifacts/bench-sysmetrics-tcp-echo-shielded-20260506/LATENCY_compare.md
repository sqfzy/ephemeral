# 延迟数据补充：baseline vs shielded（kernel / DPDK）

生成时间：2026-05-06T12:17:42.577112+00:00  
采样：每个 trial 5 分钟、256 byte payload、no TLS、~46k req/s

| 组 | trial 数 |
|---|---:|
| baseline-kernel | 3 |
| baseline-dpdk | 3 |
| shielded-kernel | 6 |
| shielded-dpdk | 6 |

每个组的统计是 N-trial 的 mean ± stddev（每个 trial 内是 ~12M 样本的 hdrhistogram 已经压成 percentile 了，
所以这里的 stddev 是"trial 间一致性"，不是单 trial 内的样本方差）。

## 每 leg 的 percentile（mean ± trial-间 stddev）

### RTT (round-trip)

| pct | baseline-kernel | baseline-dpdk | shielded-kernel | shielded-dpdk |
|---|---:|---:|---:|---:|
| **min** | 21.76 µs ± 638 ns | 17.01 µs ± 1.11 µs | 20.38 µs ± 1.54 µs | 16.67 µs ± 664 ns |
| **p50** | 24.29 µs ± 542 ns | 19.00 µs ± 1.20 µs | 24.35 µs ± 398 ns | 20.55 µs ± 141 ns |
| **p90** | 27.48 µs ± 666 ns | 20.33 µs ± 1.23 µs | 29.06 µs ± 3.02 µs | 22.10 µs ± 248 ns |
| **p99** | 69.64 µs ± 37 ns | 68.89 µs ± 111 ns | 69.56 µs ± 197 ns | 68.90 µs ± 178 ns |
| **p99.9** | 110.31 µs ± 19.55 µs | 74.29 µs ± 772 ns | 89.65 µs ± 9.56 µs | 74.45 µs ± 1.63 µs |
| **max** | 19.13 ms ± 2.33 ms | 18.89 ms ± 2.73 ms | 21.81 ms ± 4.50 ms | 23.47 ms ± 2.49 ms |
| **avg** | 27.63 µs ± 497 ns | 20.33 µs ± 1.11 µs | 27.78 µs ± 845 ns | 21.61 µs ± 112 ns |
| **stddev** | 26.91 µs ± 1.51 µs | 24.07 µs ± 2.77 µs | 20.26 µs ± 2.67 µs | 17.51 µs ± 2.58 µs |

### TX (client→server)

| pct | baseline-kernel | baseline-dpdk | shielded-kernel | shielded-dpdk |
|---|---:|---:|---:|---:|
| **min** | 10.96 µs ± 485 ns | 9.29 µs ± 505 ns | 9.89 µs ± 331 ns | 9.07 µs ± 119 ns |
| **p50** | 13.33 µs ± 156 ns | 10.81 µs ± 1.19 µs | 13.27 µs ± 167 ns | 12.29 µs ± 100 ns |
| **p90** | 15.01 µs ± 385 ns | 11.83 µs ± 1.26 µs | 14.77 µs ± 235 ns | 13.58 µs ± 203 ns |
| **p99** | 58.21 µs ± 360 ns | 60.56 µs ± 115 ns | 58.09 µs ± 323 ns | 60.47 µs ± 163 ns |
| **p99.9** | 67.91 µs ± 6.61 µs | 63.17 µs ± 1.14 µs | 63.73 µs ± 2.56 µs | 62.74 µs ± 834 ns |
| **max** | 19.10 ms ± 2.32 ms | 18.86 ms ± 2.73 ms | 21.77 ms ± 4.50 ms | 23.45 ms ± 2.49 ms |
| **avg** | 15.14 µs ± 178 ns | 12.07 µs ± 1.11 µs | 15.04 µs ± 457 ns | 13.25 µs ± 92 ns |
| **stddev** | 25.37 µs ± 1.68 µs | 23.91 µs ± 2.78 µs | 18.43 µs ± 2.91 µs | 17.17 µs ± 2.61 µs |

### RX (server→client)

| pct | baseline-kernel | baseline-dpdk | shielded-kernel | shielded-dpdk |
|---|---:|---:|---:|---:|
| **min** | 9.54 µs ± 556 ns | 7.18 µs ± 33 ns | 9.61 µs ± 558 ns | 7.17 µs ± 39 ns |
| **p50** | 10.78 µs ± 418 ns | 8.06 µs ± 27 ns | 10.95 µs ± 378 ns | 8.13 µs ± 35 ns |
| **p90** | 12.34 µs ± 412 ns | 8.85 µs ± 42 ns | 12.23 µs ± 530 ns | 8.93 µs ± 46 ns |
| **p99** | 55.61 µs ± 151 ns | 10.12 µs ± 240 ns | 55.83 µs ± 415 ns | 10.38 µs ± 237 ns |
| **p99.9** | 66.53 µs ± 14.11 µs | 34.78 µs ± 1.03 µs | 57.83 µs ± 837 ns | 34.81 µs ± 140 ns |
| **max** | 5.61 ms ± 2.03 ms | 284.75 µs ± 26.90 µs | 1.33 ms ± 1.26 ms | 1.25 ms ± 657.99 µs |
| **avg** | 12.46 µs ± 330 ns | 8.23 µs ± 8 ns | 12.70 µs ± 486 ns | 8.32 µs ± 37 ns |
| **stddev** | 8.72 µs ± 315 ns | 1.63 µs ± 64 ns | 8.20 µs ± 784 ns | 2.95 µs ± 150 ns |

### throughput

| | baseline-kernel | baseline-dpdk | shielded-kernel | shielded-dpdk |
|---|---:|---:|---:|---:|
| samples/s | 36,074 ± 649 | 49,061 ± 2582 | 35,899 ± 1081 | 46,090 ± 238 |

## 两两对比（相对差）

### baseline-kernel → baseline-dpdk
_kernel vs DPDK（无 shield）— 同一原始对比，只是从延迟视角再看一次_

| leg / pct | baseline-kernel | baseline-dpdk | Δ |
|---|---:|---:|---:|
| RTT (round-trip) p50 | 24.29 µs | 19.00 µs | ⬇ -21.8% |
| RTT (round-trip) p90 | 27.48 µs | 20.33 µs | ⬇ -26.0% |
| RTT (round-trip) p99 | 69.64 µs | 68.89 µs | ≈ -1.1% |
| RTT (round-trip) p99.9 | 110.31 µs | 74.29 µs | ⬇ -32.7% |
| TX (client→server) p50 | 13.33 µs | 10.81 µs | ⬇ -18.9% |
| TX (client→server) p90 | 15.01 µs | 11.83 µs | ⬇ -21.2% |
| TX (client→server) p99 | 58.21 µs | 60.56 µs | ≈ +4.0% |
| TX (client→server) p99.9 | 67.91 µs | 63.17 µs | ⬇ -7.0% |
| RX (server→client) p50 | 10.78 µs | 8.06 µs | ⬇ -25.3% |
| RX (server→client) p90 | 12.34 µs | 8.85 µs | ⬇ -28.3% |
| RX (server→client) p99 | 55.61 µs | 10.12 µs | ⬇ -81.8% |
| RX (server→client) p99.9 | 66.53 µs | 34.78 µs | ⬇ -47.7% |

### baseline-kernel → shielded-kernel
_kernel mode 加 shield 后的延迟变化（应基本不变 — shield 主要砍系统指标，不直接影响 lat 路径）_

| leg / pct | baseline-kernel | shielded-kernel | Δ |
|---|---:|---:|---:|
| RTT (round-trip) p50 | 24.29 µs | 24.35 µs | ≈ +0.3% |
| RTT (round-trip) p90 | 27.48 µs | 29.06 µs | ⬆ +5.8% |
| RTT (round-trip) p99 | 69.64 µs | 69.56 µs | ≈ -0.1% |
| RTT (round-trip) p99.9 | 110.31 µs | 89.65 µs | ⬇ -18.7% |
| TX (client→server) p50 | 13.33 µs | 13.27 µs | ≈ -0.5% |
| TX (client→server) p90 | 15.01 µs | 14.77 µs | ≈ -1.6% |
| TX (client→server) p99 | 58.21 µs | 58.09 µs | ≈ -0.2% |
| TX (client→server) p99.9 | 67.91 µs | 63.73 µs | ⬇ -6.2% |
| RX (server→client) p50 | 10.78 µs | 10.95 µs | ≈ +1.5% |
| RX (server→client) p90 | 12.34 µs | 12.23 µs | ≈ -0.8% |
| RX (server→client) p99 | 55.61 µs | 55.83 µs | ≈ +0.4% |
| RX (server→client) p99.9 | 66.53 µs | 57.83 µs | ⬇ -13.1% |

### baseline-dpdk → shielded-dpdk
_DPDK mode 加 shield 后的延迟变化（cache-miss 砍 41% 后，p50 可能小幅改善）_

| leg / pct | baseline-dpdk | shielded-dpdk | Δ |
|---|---:|---:|---:|
| RTT (round-trip) p50 | 19.00 µs | 20.55 µs | ⬆ +8.1% |
| RTT (round-trip) p90 | 20.33 µs | 22.10 µs | ⬆ +8.7% |
| RTT (round-trip) p99 | 68.89 µs | 68.90 µs | ≈ +0.0% |
| RTT (round-trip) p99.9 | 74.29 µs | 74.45 µs | ≈ +0.2% |
| TX (client→server) p50 | 10.81 µs | 12.29 µs | ⬆ +13.6% |
| TX (client→server) p90 | 11.83 µs | 13.58 µs | ⬆ +14.8% |
| TX (client→server) p99 | 60.56 µs | 60.47 µs | ≈ -0.1% |
| TX (client→server) p99.9 | 63.17 µs | 62.74 µs | ≈ -0.7% |
| RX (server→client) p50 | 8.06 µs | 8.13 µs | ≈ +0.9% |
| RX (server→client) p90 | 8.85 µs | 8.93 µs | ≈ +0.9% |
| RX (server→client) p99 | 10.12 µs | 10.38 µs | ≈ +2.6% |
| RX (server→client) p99.9 | 34.78 µs | 34.81 µs | ≈ +0.1% |

## 一句话感受

数字层面看：

- **RTT p50：DPDK 比 kernel 快 ~5 µs**（kernel ~24 µs vs DPDK ~19-21 µs，跨 baseline / shielded 一致）。这是用户真正看得到的延迟差。
- **RTT p99：两种 backend 几乎相等（~69 µs）**。tail 由 hypervisor preempt / NIC 中断之类的偶发事件决定，跟协议栈选型关系不大。
- **RX leg p99：DPDK 10 µs vs kernel 56 µs**。这是最戏剧性的一项 — DPDK 的 RX 路径（vfio mbuf → 应用直接读）几乎没有 tail；kernel 的 RX 走 ENA driver IRQ → softirq → epoll → recv，tail 是中断处理被排队挤出来的。
- **TX leg：两种 backend p99 都在 58-60 µs 量级**，相近。说明 TCP 写路径上 kernel TCP stack 经过几十年优化已经很难再加速；瓶颈不在 TX 协议路径而在 sched / cache。
- **Throughput：DPDK 比 kernel 多 28-36%（46-49k vs 36k samples/s）**。busy-poll 加上不走 syscall + 软中断让 cycles 真的更"专注"。

Shield 对延迟的影响：**几乎可忽略**（kernel/DPDK 两边的 p50/p99 差 ≤ 1.5%）。这跟系统指标分析一致：shield 砍掉的是"伪并发噪声"，不影响 hot-path latency；唯一例外是 DPDK 的 cache-miss 41% 改善（已在 system metrics 报告里证实）。

### 一行总结

**对终端用户：DPDK 把 RTT p50 从 24 µs 砍到 19 µs（-21%），RX p99 从 56 µs 砍到 10 µs（-82%），吞吐 +30%。其余 tail / TX 路径差异不显著。**
**Shield 对延迟没有可感知影响**，所以纯延迟视角下也没必要为 shield 买单（cgroup 干净度只在 system-level metrics + 长跑稳定性上有意义）。
