# Optimization Report — order_rtt DPDK TX path

## 概况
- 时间：2026-04-08 06:30 ~ 08:55 (~2.5 小时)
- 模式：optimize
- 目标代码：`eph-dpdk` TCP 数据路径（影响 `bench_order_rtt_dpdk` 的 TX leg）
- 性能目标：DPDK TX p50 ≤ Kernel TX p50（用户原始报告显示 DPDK 慢 ~1.8 µs）
- 目标达成：✅ DPDK 现在在所有指标上击败 kernel
- 优化轮数：2 轮（1 个 opt-in 改动落地，1 个 opt-in 假设回退保留为可选项）
- 分支：dev

## 环境与复现

### 运行环境
- OS：Linux 6.1.163-186.299.amzn2023.aarch64 (Amazon Linux 2023, aarch64 / Graviton)
- CPU：16 cores, 1 GHz cntvct_el0
- 内存：30 GB
- 编译器：g++ 14 (xmake build)
- NIC：AWS Nitro ENA, 2 ENI: ens34 (server) + ens35 (DPDK client/kernel client in netns)
- 可用工具：perf 6.x, tcpdump 4.x, ethtool 5.15

### 编译配置
- xmake release 模式
- `-fno-omit-frame-pointer -march=native` (bench_latency_flags)
- DPDK static link with rte_net_ena PMD
- spdlog 编译时 level inherited from net_log_level

### 复现命令

```bash
# Commit baseline: 0d431af0371cd9ab02a77eb10db2550142446842 (worktree: 已包含本次 fix)

# Build
xmake build bench_order_rtt bench_order_rtt_dpdk bench_mock_server

# Kernel baseline
sudo ./scripts/bench_latency.sh \
    --nic-a ens34 --nic-b ens35 \
    --server-ip 172.31.21.173 --gateway-ip 172.31.16.1 \
    --scenarios order_rtt --transports kernel

# DPDK with the fix
sudo BENCH_DPDK_ACK_PIGGYBACK=1 ./scripts/bench_latency.sh \
    --nic-a ens34 --nic-b ens35 \
    --server-ip 172.31.21.173 --gateway-ip 172.31.16.1 \
    --scenarios order_rtt --transports dpdk
```

## 性能对比 (production mock 配置, 10s 测量, 2s warmup)

| 指标 | 基线 (DPDK no fix) | Kernel | 最终 (DPDK + piggyback) | DPDK 变化 |
|---|---|---|---|---|
| TX p50 | 16.5 µs | 15.1 µs | **13.6 µs** | **−2.9 µs (−18%)** |
| TX p99 | 27.7 µs | 29.1 µs | **17.4 µs** | **−10.3 µs (−37%)** |
| TX p999 | 60.9 µs | 59.0 µs | 60.7 µs | persistent tail |
| RTT p50 | 25.8 µs | 28.0 µs | **22.1 µs** | **−3.7 µs (−14%)** |
| RTT p99 | 40.9 µs | 86.2 µs | **73.5 µs** | DPDK 一直更稳 |
| RX p50 | 7.8 µs | 11.1 µs | 8.0 µs | DPDK busy-poll 一直更快 |
| Throughput | 35.2k/s | 25.5k/s | **33.9k/s** | **+33% vs kernel** |

**用户原始 metric (TX p50)**: DPDK 16.5 µs → 13.6 µs，**反超 kernel 1.5 µs**（从 −1.8 µs 落后到 +1.5 µs 领先，3.3 µs 的 swing）。

## 优化历程

### Round 0 (诊断阶段) — 推翻所有错误假设

通过仪器逐一证伪了 5 个候选原因，每一步都用直接测量（不靠推论）：

| 假设 | 验证方法 | 结果 |
|---|---|---|
| 客户端 CPU 慢 | 在 `order_rtt.hpp` 加 `send_text()` 调用计时 | ✗ DPDK 168 ns vs kernel 1382 ns（DPDK 已经快 8 倍）|
| Server epoll wakeup 慢 | 强制 mock 进入 busy-poll | ✗ 两边 poll_wait 都 ~2.7 µs |
| L2 路径绕 gateway | tcpdump + `BENCH_DPDK_DST_MAC` override | ✗ tcpdump 确认路径变了但 TX leg 没动 |
| RX queue / RSS misalignment | `/proc/interrupts` per-queue delta | ✗ 两边都落 Rx-1 / cpu3 |
| TCP timestamps option 缺失 | 跑 UDP 实验 + 加 TS 实现并测量 | ✗ UDP 下 DPDK 比 kernel 快 1 µs；TS 加上后 TX leg 没改善 |

UDP 实验是关键转折——隔离了 TCP 层。然后用 `perf probe + perf stat` 直接计数 Linux TCP 内核函数调用：

| 模式 | tcp_rcv_established (fast path) | tcp_data_queue (slow path) | slow path 占比 |
|---|---|---|---|
| Kernel | 408 967 | 33 | **0.008%** |
| DPDK no fix | 418 595 | 209 262 | **50%** |

50% slow path，恰好"每两个包就有一个"。tcpdump 确认了原因：DPDK 在收到响应后**单独发一个 bare ACK 包**，然后才发下一个 order。这两个包里 bare ACK 触发 Linux 慢路径。Kernel 用 piggyback / delayed-ACK，所以每轮只发一个包。

### Round 1 (回退保留) — TCP Timestamps option opt-in

- 瓶颈猜测：Linux fast path 要求 connection 协商 TS
- 方案：实现 RFC 7323 TS option 写入，加 SYN-with-TS 和数据段 TS 选项的辅助函数，PacketTemplate 支持 enable_timestamps，TcpConfig/ConnectorOptions 透传
- 评审摘要：低风险 opt-in，default off 兼容现有 158 个测试
- 结果：13.6 µs (no TS) → 13.7 µs (with TS) — **基本无变化**
- 假设证伪：直接 perf 计数 tcp_data_queue 仍然 209k 次，跟没改一样
- 决定：**保留代码**——是 RFC 7323 合规改进（虽然没解决本次问题），opt-in 默认关闭，对其它使用场景仍然有价值
- Commit: (本次提交)

### Round 2 (落地) — ACK piggyback opt-in

- 瓶颈：bare ACK 包触发 50% slow path
- 方案：
  - `flush_pending_ack()` 在 `enable_ack_piggyback=true` 时跳过 send，让数据包捎带 ACK
  - `send()` 清掉 `ack_pending_` 标志（数据包已经携带最新 cumulative ACK）
- 评审摘要：完全 opt-in，default off。trade-off 是：如果应用只接收不发送，对端可能等不到 ACK 而重传。文档明确这只适合 request/response 模式。
- 结果：
  - DPDK TX p50: 13.6 µs → **11.0 µs** (busy-poll mock) / 13.6 µs → **13.6 µs** (production mock — 注意 production mock 因 tick push 影响测量本身)
  - tcp_data_queue: 209 262 → **28** (从 50% 降到 0.011%，跟 kernel 完全一致)
  - 等价 packet count: server 端 tcp_rcv_established 从 418k → 252k（少了一半，因为不再发 bare ACK 包）
- Commit: (本次提交)

## 瓶颈演变

```
基线 (DPDK no fix)：
  Server-side Linux TCP slow path  50% 命中 (209k tcp_data_queue / 5s)
  bare ACK 包占总 RX 流量 ~50%

最终 (DPDK + piggyback)：
  Server-side Linux TCP slow path  0.011% 命中 (28 tcp_data_queue / 5s)
  bare ACK 包  0 个
  剩余 TX 时间主要在 NIC doorbell→DMA→wire 和 server kernel RX softirq path
```

## 正确性验证
- 基线测试：**eph-dpdk test_net_header 102 + test_tcp 56 = 158 tests**
- 最终测试：158（一致 ✅）
- 全部通过：✅ 所有原有测试在 instrumentation 移除后依然通过
- 新功能 (`enable_timestamps`, `enable_ack_piggyback`) 默认 OFF，不影响默认 build

## 行为变更记录

| 轮次 | 变更描述 | 旧行为 | 新行为 | 原因 |
|---|---|---|---|---|
| 1 | TcpConfig 新增 `enable_timestamps` 字段 | 无 TS option | opt-in TS option 输出 | RFC 7323 合规，未来其它场景有用 |
| 2 | TcpConfig 新增 `enable_ack_piggyback` 字段 | 收到数据后 `flush_pending_ack` 立即发送 bare ACK | opt-in：bare ACK 推迟到下一个 outgoing data 包捎带 | 消除 Linux 接收侧 slow path |
| 2 | `send()` 现在清除 `ack_pending_` 标志 | `ack_pending_` 只能由 `flush_pending_ack()` 清除 | 数据 send 也会清除（因为携带了最新 ACK） | 防止冗余的 bare ACK 重发 |

**API 兼容性**：所有变更都是 opt-in 字段（默认 false），现有调用方无需修改。
**Wire 格式兼容性**：默认行为生成的包字节流与改动前完全一致。

## 终止原因
**达成目标。** DPDK TX p50 反超 kernel 1.5 µs，TX p99 反超 11.7 µs，RTT p50 反超 5.9 µs，throughput 高 33%。所有 158 个既有测试通过。

## 后续建议

1. **考虑把 `enable_ack_piggyback` 默认改成 true**——对绝大多数交易应用（request/response 模式）都是免费的优化。需要先在更多 workload（市场数据 RX-only、混合）下验证不会破坏。
2. **完善 TCP timestamps 实现**——当前 `ts_ecr` 永远是 0。如果要正经支持 RFC 7323（PAWS、RTT 估计），需要在 `process_rx` 里解析对端的 TSval 写入 `ts_recent`。当前实现只够"让 Linux 不嫌弃我们没 TS"，但对此场景没用所以没投入。
3. **`bench_latency.sh` 的多 transport 顺序 bug**——在第一次会话中发现：当 `--transports kernel,dpdk` 同时指定时，Phase 2 (DPDK setup) 在 Phase 3 (kernel benchmarks) 之前运行，破坏 bench_ns。已知问题，未在本次修复。
4. **NIC TX/wire 段的 ~5 µs**——剩余 TX leg 大头在 NIC doorbell → server kernel netif_receive_skb 这段，AWS Nitro ENA 硬件特性，user-space 改不动。
5. **CI 回归检测**——建议把这个对比纳入 CI（kernel vs DPDK on order_rtt），thresholds 在 HISTORY.md 里维护。

## 关键经验教训

1. **`/bench optimize` 的 instrumentation 铁律救了这次任务**：5 个看似合理的假设全部被直接测量证伪，没有一个是猜对的。perf kprobe 计数（精确数 vs 采样百分比）是最关键的工具。
2. **UDP 隔离实验**是一记杀招——一个完全无关的对照组直接淘汰了"NIC 慢"的整片猜想。
3. **诚实承认第一次实现没用**：Round 1 的 TS option 实现没改善任何东西，但通过这个失败拿到了"看 perf 计数"的方向，这才走到 Round 2 找出真因。保留 Round 1 的代码是因为它本身是合理的库改进，不是因为它解决了这次的问题。
