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

## 对比

| 指标 | Run A (1q) | Run B (4q + RETA collapse) | Δ |
|---|---:|---:|---|
| RTT p50  (ns)   | 20231 | 20055 | -0.9% ✓ within noise |
| RTT p99  (ns)   | 23223 | **70173** | **+202%** 🔴 |
| RTT p99.9 (ns)  | 26183 | 78109 | +198% 🔴 |
| TX p50   (ns)   | 12107 | 12019 | -0.7% ✓ |
| TX p99   (ns)   | 14811 | **61422** | **+315%** 🔴 |
| RX p50   (ns)   | 7977 | 7929 | -0.6% ✓ |
| RX p99   (ns)   | 9379 | 9803 | +4.5% ✓ |
| Throughput      | 48944 s/s | **33385 s/s** | **-32%** 🔴 |

## 关键发现

1. ✅ **Correctness preserved** — single stream attach succeeds, TCP echo round trip works under both configurations. The PR-2.5 RETA-collapse fix is doing its job: incoming packets land on queue 0 where the single Poller picks them up.

2. ✅ **p50 latency unaffected** — median latency under multi-queue is within noise of single-queue. Hot-path round trip cost is identical.

3. 🔴 **Tail latency catastrophe** — RTT p99/p99.9 explode 3× under multi-queue. The TX leg is the worst offender (TX p99 +315%), suggesting **per-send NIC overhead** scales with `nb_tx_queues` (we set `nb_tx_queues = nb_rx_queues = 4`). Likely cause: `rte_eth_tx_burst` or the ENA PMD's per-send doorbell ringing iterates over all configured TX queues even when only one is in use.

4. 🔴 **Throughput -32%** — at sustained load, the multi-queue overhead becomes throttling. Lower throughput correlates with the elevated tail latency.

5. **NIC capability gap on ENA**: `rte_eth_dev_rss_hash_update` unsupported means PR-8's intended demo (N parallel streams across N queues with controllable RSS) **cannot be exercised on AWS ENA**. RETA collapse is correct fallback for single-stream case but eliminates any throughput-distribution benefit. The multi-queue throughput improvement story requires Mellanox / Intel NICs.

## 工程结论

| 用例 | 推荐 config | 理由 |
|---|---|---|
| **Single-stream HFT (lat_tcp / lat_ws / 大多数 lat scenario)** | `NB_RX_QUEUES=1` (默认) | Multi-queue 增加 TX 开销，p99 tail 翻倍以上，没收益 |
| **Multi-stream HFT** (PR-8 设想) | `NB_RX_QUEUES=N` + ENA: 等价于 RETA-collapse 集中到 q0 (无收益)；Mellanox/Intel: 真分散到 N 个 lcore (收益) | NIC-dependent |
| **ENA 上的多 stream** | 不要开 `NB_RX_QUEUES > 1` | RSS 不可控 → RETA collapse → 所有 stream 仍挤 queue 0 → 比 1-queue 还慢 |

## 后续建议

1. **bench.conf 默认值已经正确** — `NB_RX_QUEUES=1, ENABLE_RSS=false` 是最佳通用配置。文档已警告 LCORE_PER_QUEUE 字段是 reserved (commit `4ef72a3` 中 PR-5 surgical doc patch)。

2. **PR-8 真正的 multi-stream throughput 验证** 需要 RSS-controllable NIC（Mellanox ConnectX 或 Intel E810）。代码层面 (`Platform::register_poller` × N + `Stream::create_and_attach` with `pin_to_queue=i` + N lcores) 已经 ready；只需在 RSS-capable NIC 上跑实际多 stream lat scenario 即可观测预期收益。

3. **TX queue 开销调查** (新发现的问题) — 单 stream 用 4 TX queue 时 TX p99 +315% 不应该。值得作为独立 perf 分析任务深挖 ENA PMD 的 tx_burst 实现。

4. **平台代码加 hard guard** — Platform::create 在 `dispatch_mode=Software` + `nb_rx_queues > 1` 时不仅 collapse RETA，应该也 emit WARN 提示用户"unless multi-stream is intended, set nb_rx_queues=1 for better tail latency"。当前只在 INFO log 输出。可加一个新 PR。

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
| 8 | (this report) | ⚠️ | NIC limitation surfaces — single-stream multi-queue is a perf trap on ENA, RSS-multi-stream demo deferred to RSS-capable NIC environment |

**13 commits ahead of pre-RSS main**, all build green, all single-stream lat scenarios zero regression in default config, real-NIC integration verified with M2 invariant + RETA collapse + create_and_attach turnkey.
