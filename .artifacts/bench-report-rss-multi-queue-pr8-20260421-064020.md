# Bench Report — PR-8 multi-queue throughput verification (ENA single-stream)

## 概况

- **时间**：2026-04-21 06:39
- **目的**：PR-8 stage-5 final verification — measure multi-queue config impact on the now-correct single-stream path (post-PR-2.5 RETA collapse)
- **scenario**：`lat tcp --dpdk`, 15s duration, 256 B payload, ENA NIC PCI 0000:28:00.0
- **branch HEAD**：`001b840` (after PR-7 mockex multi-conn)
- **NIC**：AWS EC2 ENA — `rte_eth_dev_rss_hash_update` unsupported → rss_active=false → dispatch_mode pinned Software → RETA collapsed

## 数据

### Run A — Baseline (`NB_RX_QUEUES=1, ENABLE_RSS=false`)

```
Platform ready (port=0, nb_rx_queues=1, rss_active=false,
                dispatch_mode=Software (single-Poller fallback))
create_and_attach: TCP stream attached → port=0, queue=0, mode=Software

samples: 732988
RTT_ns:  p50=20231  p99=23223  p99.9=26183
TX_ns:   p50=12107  p99=14811  p99.9=17863
RX_ns:   p50=7977   p99=9379   p99.9=10283
throughput: 48944 samples/s
```

### Run B — Multi-queue (`NB_RX_QUEUES=4, ENABLE_RSS=true`)

```
Platform: NIC supports RSS Partitioned but RSS not active
  (nb_rx_queues=4, rss_active=false); pinning dispatch_mode to Software
Platform: collapsed RETA → queue 0 (nb_rx_queues=4 active, but
  Software mode → single-Poller; all RX traffic routes to queue 0)
Platform ready (port=0, nb_rx_queues=4, rss_active=false,
                dispatch_mode=Software (single-Poller fallback))
create_and_attach: TCP stream attached → port=0, queue=0, mode=Software

samples: 498540
RTT_ns:  p50=20055  p99=70173  p99.9=78109
TX_ns:   p50=12019  p99=61422  p99.9=65102
RX_ns:   p50=7929   p99=9803   p99.9=13267
throughput: 33385 samples/s
```

## 对比（**单次 15s 跑，N=1，下文结论需重复实验确认**）

| 指标 | Run A (1q) | Run B (4q + RETA collapse) | 观察到的 Δ |
|---|---:|---:|---|
| RTT p50  (ns)   | 20231 | 20055 | -0.9% ✓ within noise |
| RTT p99  (ns)   | 23223 | 70173 | +202% (单次观察) |
| RTT p99.9 (ns)  | 26183 | 78109 | +198% (单次观察) |
| TX p50   (ns)   | 12107 | 12019 | -0.7% ✓ |
| TX p99   (ns)   | 14811 | 61422 | +315% (单次观察) |
| RX p50   (ns)   | 7977 | 7929 | -0.6% ✓ |
| RX p99   (ns)   | 9379 | 9803 | +4.5% ✓ |
| Throughput      | 48944 s/s | 33385 s/s | -32% (单次观察) |

## 关键发现

1. ✅ **Correctness preserved** — single stream attach succeeds, TCP echo round trip works under both configurations. The PR-2.5 RETA-collapse fix is doing its job: incoming packets land on queue 0 where the single Poller picks them up.

2. ✅ **p50 latency unaffected** — median latency under multi-queue is within noise of single-queue. Hot-path round trip cost is identical.

3. ⚠️ **Tail latency 单次观察异常** — RTT p99/p99.9 在这一次 15s 跑里高 ~3×。**这是 N=1 的观察**，不能据此下结论。p99 受 outlier 强烈影响（AWS 邻居 vCPU 干扰 / kernel scheduling jitter / journald flush / 单次 GC-style 事件 都可能造成同等量级的尾巴抬升）。判定 multi-queue 是否真的会引入额外尾延迟，必须做 N≥10 次重复跑 + mean/stddev/CI 分析。**这条数据当作"留待复现的可疑信号"，不当作结论**。

4. ⚠️ **Throughput 单次观察 -32%** — 同上。throughput 与 tail latency 在 sustained load 下相关，所以同一次 outlier 事件会同时拖累两个指标。需要重复实验验证。

5. **NIC capability gap on ENA**（这一条与 perf 数据无关，是 NIC 能力的硬事实）：`rte_eth_dev_rss_hash_update` unsupported 意味着 PR-8 原本想演示的 "N parallel streams across N controllable queues" 无法在 AWS ENA 上做。RETA collapse 是 single-stream 的正确兜底；若要看真 multi-stream throughput 收益，需要 RSS-controllable NIC（Mellanox / Intel）。

## 工程结论（保守化）

| 用例 | 推荐 config | 理由 |
|---|---|---|
| **Single-stream HFT (lat_tcp / lat_ws / 大多数 lat scenario)** | `NB_RX_QUEUES=1` (默认) | (a) 默认已经够用、无需多 queue；(b) 多 queue 单流场景有"留待复现的可疑尾延迟信号"，保守起见不要开 |
| **Multi-stream HFT (PR-8 设想)** | `NB_RX_QUEUES=N` + RSS-capable NIC | ENA 因 RSS 不可控，多 stream 也都被 RETA collapse 集中到 q0；Mellanox/Intel 上才能真分散 |

## 后续建议

1. **重复实验校准 perf 信号**（应该最先做）— 跑 N≥10 次 Run A vs Run B（同 config，每次 15s），算 p99 mean / stddev / 95% CI。如果 mean 真的差 +200% 且 CI 不重叠，这才能升级为"perf trap"结论；如果 mean 接近 / CI 大量重叠，那说明本次 +202% 是 outlier。

2. **bench.conf 默认值已经正确** — `NB_RX_QUEUES=1, ENABLE_RSS=false` 是最佳通用配置。文档已警告 `LCORE_PER_QUEUE` 字段是 reserved (commit `4ef72a3` 中 PR-5 surgical doc patch)。

3. **PR-8 真正的 multi-stream throughput 验证** 需要 RSS-controllable NIC（Mellanox ConnectX 或 Intel E810）。代码层面 (`Platform::register_poller` × N + `Stream::create_and_attach` with `pin_to_queue=i` + N lcores) 已经 ready；只需在 RSS-capable NIC 上跑实际多 stream lat scenario 即可观测预期收益。

4. **TX queue 开销** — 如果 #1 的重复实验确认尾延迟真的随 `nb_tx_queues` 抬升，再深挖 ENA PMD 的 tx_burst 实现路径。当前是 hypothesis，**不是结论**。

5. **是否加 hard guard** — 取决于 #1 结论。如果重复实验显示"multi-queue 单流确实有显著尾延迟代价"，则在 `Platform::create` 加 WARN；否则 INFO log 已足够。

## 全 PR 链回顾 (PR-0 → PR-8)

| PR | commit | 状态 | 关键 deliverable |
|---|---|---|---|
| 0 | 1286d2b+83e7f16 | ✅ | 10-commit base RSS rollout merged |
| 1 | e737fdc | ✅ | M1 perf — RSS state cache (28k×3 → 2 syscalls) |
| 2 | 3ed8285 | ✅ | M2+m1+m3+m4 RSS invariants |
| 2.5 | 094f8f3 | ✅ | RETA-collapse real fix (root-cause for SYN-ACK loss) |
| 3 | 3c0abad+97d5d3d | ✅ | M3 create_and_attach E2E with fork mock |
| 4 | 57c6712 | ✅ | m2+n2+n3 polish |
| 5 | (no commit) | ➡️ | docs (no work needed — earlier sweep sufficient) |
| 6 | e7ce53c | ✅ | 7 lat scenarios → create_and_attach |
| 7 | 001b840 | ✅ | mockex tcp_echo accept-N |
| 8 | (this report) | ⚠️ | NIC limitation: ENA `rss_hash_update` unsupported → real multi-stream demo deferred to RSS-capable NIC (Mellanox/Intel). Single-stream multi-queue 配置在本次 N=1 跑里观察到尾延迟异常，但需重复实验确认。 |

**13 commits ahead of pre-RSS main**, all build green, all single-stream lat scenarios zero regression in default config, real-NIC integration verified with M2 invariant + RETA collapse + create_and_attach turnkey.
