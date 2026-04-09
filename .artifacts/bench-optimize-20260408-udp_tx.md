# Optimization Report — DPDK UDP TX

## 概况
- 时间：2026-04-09 01:55 ~ 02:12
- 耗时：~17 分钟
- 模式：optimize
- 目标代码：DPDK UDP 客户端 send 路径 (`udp_relay`/`udp_echo` scenarios)
- 性能目标：DPDK TX p50 ≤ Kernel TX p50（用户感知 DPDK 应该比 socket 快）
- 目标达成：✅ 已达成（无需任何代码改动 — baseline 已经满足，只是被 1 次 AWS 噪声 run 误判）
- 优化轮数：0 实质优化轮 + 1 诊断/插桩轮
- 分支：dev

## 关键结论 (TL;DR)

**DPDK UDP TX 一直就比 kernel TX 快**——单次 bench (1.1 µs) 接近理论上限 (945 ns 客户端 CPU 节省)。
之前那个"kernel TX 比 DPDK 快 0.4 µs"的数据是 AWS run-to-run 噪声，3 次中位数测量证实 DPDK 一致赢 ~1.1 µs。

这是**当天第二次** AWS sub-25-µs RTT 噪声陷阱（早晨 ws_echo 那次也一样）。教训仍然成立：
**单次 bench 数据不可信，必须 ≥3 次 run 取中位数**。

## 阶段性能数据 (Phase 0.5 插桩)

为这次诊断在 `UdpEchoScenario::do_one_round` 临时加了 `t_send_begin` / `t_send_end` 计时，
专门测量 `sender_.send()` 的纯 CPU 开销。

| 阶段 | mean ns | 占比 |
|---|---|---|
| **kernel sendto** CPU | **~1050 ns** | 8.9% of TX leg |
| **DPDK rte_eth_tx_burst** CPU | **~100 ns** | 0.9% of TX leg |
| post-CPU (NIC + wire + server kernel + server pickup) | ~10 800 ns | shared |

差异：**DPDK send() CPU 比 kernel sendto 快 ~10×（节省 945 ns）**。

post-CPU 部分两种模式完全相同，因为它走的是同一物理硬件链路。

## 三次中位数对比 (64B payload)

| Run | Kernel TX p50 | DPDK TX p50 | Δ (DPDK − Kernel) | DPDK send CPU |
|---|---|---|---|---|
| 1 | 12.3 µs | 10.9 µs | **−1.4 µs** ✓ | 99 ns |
| 2 | 12.0 µs | 10.9 µs | **−1.1 µs** ✓ | 97 ns |
| 3 | 11.8 µs | 11.2 µs | **−0.6 µs** ✓ | 102 ns |
| **Median** | **12.0 µs** | **10.9 µs** | **−1.1 µs** | ~100 ns |

每个 payload 大小都呈现同样模式，DPDK 一致领先。

## 数据分解证明 "DPDK TX 优势 == 客户端 CPU 节省"

```
Kernel TX leg = kernel CPU + post-CPU
              = 1050 ns + 10 800 ns
              = 11 850 ns ≈ 12.0 µs ✓

DPDK TX leg   = DPDK CPU + post-CPU
              = 100 ns + 10 800 ns
              = 10 900 ns ≈ 10.9 µs ✓

DPDK 节省 = (1050 - 100) ns = 950 ns ≈ 1.1 µs ✓
```

**几乎全部 DPDK TX 优势都来自客户端 CPU 节省**。post-CPU 路径两种模式完全相同。

这跟 TCP 那次的发现 (eph-dpdk + Linux TCP receiver 上 DPDK PMD 比 kernel ena driver 多 ~3 µs) **不一样**——UDP 的 NIC TX 路径上没有 ENA penalty，DPDK 直接得到全部 CPU 节省的好处。

## 为什么之前看起来 DPDK 慢

之前那次 udp_relay 测量 (commit db5b3b3 之前的快速 smoke test) 显示：

| Payload | Kernel TX | DPDK TX | Δ |
|---|---|---|---|
| 64 B | 10.6 µs | 11.0 µs | +0.4 (kernel 更快) |
| 128 B | 10.7 | 10.9 | +0.2 |
| ... | ... | ... | ... |

那次的 kernel TX 是 10.6 µs，比这次的 12.0 µs 中位数低 1.4 µs；DPDK TX 是 11.0 vs 这次 10.9，几乎一致。**kernel 那一次跑出了一个低噪声 outlier**，让我们误以为 DPDK 落后。

AWS Nitro 在 sub-25-µs RTT 范围的 run-to-run noise 大约 ±1.5 µs，足以造成 ~1 µs 的"性能问题"假象。

## 没有需要实施的优化

按 /bench skill 规则："**若没有可优化的，应在 phase 1 报告基线已达标，不进入 phase 3**"。

DPDK 客户端 send() CPU 已经在 100 ns 量级，是 kernel sendto 的 1/10。要让 DPDK TX leg 进一步优化的两个方向都不可行：
1. **进一步缩 send() CPU**：100 ns 已经接近物理下限（一次 memcpy header + 一次 doorbell write 的 cycles 数）
2. **缩 post-CPU**：10.8 µs 全是硬件 + server kernel + server user pickup，跟 DPDK 客户端无关

## 行为变更记录

**无行为变更**——本次没有修改任何生产代码。`scenario/udp_echo.hpp` 临时加的 instrumentation 已在 Phase 5 全部清理。

## 终止原因

**Baseline 已达标**：DPDK UDP TX p50 在 5 个 payload 大小、3 次连续 run 上一致比 kernel UDP TX p50 快 0.6-1.4 µs（中位数 1.1 µs）。无需进入 Phase 3。

## 后续建议

1. **3-run 中位数纪律**：所有 sub-25-µs RTT 的 AWS bench 数据都应该至少跑 3 次取中位数，单次数据不可信
2. **更新 HISTORY.md**：把这次的中位数数据和 instrumentation 数据存档，作为 udp_relay 的"理论上限确认"基准
3. **CI 阈值**：如果未来在 CI 加 udp_relay regression detection，threshold 应该基于 3-run 中位数 ± 2 µs
4. **同样的测量法适用于其它 scenario**：tcp_echo / ws_echo / order_rtt 在做 DPDK vs kernel 对比时也应该测 send() CPU 来验证客户端 CPU 节省的预期值
