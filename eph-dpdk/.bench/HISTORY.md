# eph-dpdk Benchmark History

## 2026-04-03 Baseline (ARM64 Graviton3, 16 cores)

Platform: Linux aarch64, 16x 2000 MHz, L1d 64K, L2 2M, L3 36M

### bench_tcp_header

| Benchmark | Time (ns) | Throughput |
|---|---|---|
| BM_Checksum/64 | 3.43 | 17.4 Gi/s |
| BM_Checksum/256 | 9.07 | 26.3 Gi/s |
| BM_Checksum/512 | 21.6 | 22.1 Gi/s |
| BM_Checksum/1024 | 57.6 | 16.6 Gi/s |
| BM_TcpHeaderBuild/64 | 28.6 | - |
| BM_TcpHeaderBuild/256 | 45.6 | - |
| BM_TcpHeaderBuild/512 | 70.9 | - |
| BM_TcpHeaderBuild/1024 | 113 | - |
| BM_TcpHeaderParse/* | 0.97 | - |
| BM_ParsePacketReal/* | 1.93 | - |
| BM_TcpChecksum/64 | 3.49 | 22.4 Gi/s |
| BM_TcpChecksum/1024 | 59.3 | 16.4 Gi/s |
| BM_Ipv4ParseFormat | 96.3 | - |
| BM_ReactorHashTuple | 1.95 | - |
| BM_ReactorDispatchSim/1 | 1.34 | - |
| BM_ReactorDispatchSim/4 | 2.74 | - |
| BM_ReactorDispatchSim/16 | 11.6 | - |

## 2026-04-03 Batch 2 (new benchmarks added)

Platform: same as baseline

### bench_tcp_header (new entries)

| Benchmark | Time (ns) | Notes |
|---|---|---|
| BM_WriteSynOptions | 0.405 | SYN option serialization (12 bytes) |
| BM_ConnectionTupleDump | 333 | Diagnostic formatting |
| BM_ParsedPacketDump | 537 | Diagnostic formatting (flags + addresses) |

No regressions vs baseline (all existing benchmarks within noise).

| Benchmark | Time (ns) | Notes |
|---|---|---|
| BM_PacketTemplateDump | 832 | Diagnostic: MACs + IPs + MSS + hw_cksum |
| BM_PacketTemplateValidate | 0.36 | Constexpr-capable field validation |

### bench_dns_codec

| Benchmark | Time (ns) |
|---|---|
| BM_EncodeQname | 32.9 |
| BM_BuildDnsQuery | 31.1 |
| BM_ParseDnsResponse | 12.9 |
| BM_SkipDnsName | 3.93 |

### bench_multicast

| Benchmark | Time (ns) | Throughput |
|---|---|---|
| BM_ParseUdpPacket/0 | 7.10 | 5.5 Gi/s |
| BM_ParseUdpPacket/1460 | 7.11 | 196.9 Gi/s |
| BM_MulticastMacFromIp | 1.12 | - |
| BM_IsMulticastIp | 0.505 | - |
| BM_ParseUdpPacketRejectTcp | 1.10 | - |

## 2026-04-03 Batch 3 (observability + design improvements)

Platform: same as baseline

### bench_tcp_header (new entries)

| Benchmark | Time (ns) | Notes |
|---|---|---|
| BM_FlowRuleDump | 86.9 | FlowRule diagnostic formatting |
| BM_FlowRuleToJson | 126 | FlowRule JSON serialization |
| BM_TcpConfigDump | 883 | TcpConfig multi-line dump (MACs + IPs) |
| BM_TcpConfigToJson | 911 | TcpConfig JSON (12 fields) |
| BM_TcpStatsDump | 673 | TcpSession::Stats diagnostic dump |
| BM_TcpStatsToJson | 595 | TcpSession::Stats JSON (sparse histogram) |
| BM_ParsedPacketToJson | 565 | Parsed TCP packet JSON |
| BM_ConnectionTupleToJson | 359 | 4-tuple JSON serialization |
| BM_ReactorConfigDump | 115 | Reactor::Config diagnostic dump |
| BM_ReactorConfigToJson | 125 | Reactor::Config JSON |

### bench_dns_codec (new entries)

| Benchmark | Time (ns) | Notes |
|---|---|---|
| BM_ResolveHostnameDottedDecimal | 23.2 | Fast-path IP string parsing |

### bench_multicast (new entries)

| Benchmark | Time (ns) | Notes |
|---|---|---|
| BM_ParsedUdpPacketDump | 411 | UDP packet diagnostic dump |
| BM_ParsedUdpPacketToJson | 406 | UDP packet JSON serialization |

No regressions vs baseline (all existing benchmarks verified stable).

## 2026-04-07 rte_memcpy A/B test (ARM64 Graviton3, bench_latency.sh)

Platform: Linux aarch64, 16 cores, AWS EC2, dual ENA NIC (ens34+ens35)
Duration: 10s per test, DPDK only (skip-socket)
Build: xmake release, GCC 14, `-march=native`

### bench_market_dpdk (Market Data Pipeline Latency)

| Metric | Baseline (std::memcpy) | rte_memcpy | Delta |
|--------|----------------------|------------|-------|
| samples | 28362 | 28362 | - |
| min | 6476 ns | 6395 ns | -1.3% |
| p50 | 7948 ns | 8044 ns | +1.2% |
| p99 | 11236 ns | 11660 ns | +3.8% |
| p99.9 | 18056 ns | 17112 ns | -5.2% |
| max | 2938340 ns | 32895 ns | -98.9%* |

\* max 差异来自单次 outlier（baseline 有 ~3ms spike），不具统计意义。

### bench_order_rtt_dpdk — Order RTT (send -> response recv)

| Metric | Baseline (std::memcpy) | rte_memcpy | Delta |
|--------|----------------------|------------|-------|
| samples | 10000 | 10000 | - |
| min | 20679 ns | 20305 ns | -1.8% |
| p50 | 22888 ns | 22088 ns | -3.5% |
| p99 | 38288 ns | 29816 ns | -22.1% |
| p99.9 | 203712 ns | 145984 ns | -28.3% |
| max | 983264 ns | 778533 ns | -20.8% |

### bench_order_rtt_dpdk — Response Latency (mock send -> app recv)

| Metric | Baseline (std::memcpy) | rte_memcpy | Delta |
|--------|----------------------|------------|-------|
| samples | 10000 | 10000 | - |
| min | 7248 ns | 7008 ns | -3.3% |
| p50 | 8284 ns | 7988 ns | -3.6% |
| p99 | 15300 ns | 10804 ns | -29.4% |
| p99.9 | 55504 ns | 17592 ns | -68.3% |
| max | 100284 ns | 56402 ns | -43.8% |

### 分析

**Market data (bench_market_dpdk)**: p50/p99 在噪声范围内（±4%）。rte_memcpy
对有序流量无影响——符合预期，因为 reorder path 的 memcpy 仅在乱序 segment 触发。

**Order RTT**: p99 改善 22%，p99.9 改善 28%。rte_memcpy 版本在 tail latency 上有
显著优势。但需注意：单次 10s 运行的 p99/p99.9 样本量有限（100/10 个点），结果可能
受运行间噪声影响。建议多轮运行取中位数确认。

**10s 结论**：P99 tail 改善显著但样本量有限，需 60s 验证。

### 60s 复测 (170K market samples, 60K order samples)

#### bench_market_dpdk (Market Data Pipeline Latency)

| Metric | Baseline (std::memcpy) | rte_memcpy | Delta |
|--------|----------------------|------------|-------|
| samples | 170187 | 170211 | - |
| min | 6356 ns | 6440 ns | +1.3% |
| p50 | 8420 ns | 7996 ns | -5.0% |
| p99 | 13476 ns | 11028 ns | -18.2% |
| p99.9 | 18392 ns | 15588 ns | -15.2% |
| max | 79499 ns | 33509 ns | -57.8% |

#### bench_order_rtt_dpdk — Order RTT (send -> response recv)

| Metric | Baseline (std::memcpy) | rte_memcpy | Delta |
|--------|----------------------|------------|-------|
| samples | 60000 | 60000 | - |
| min | 10282 ns | 15729 ns | +53.0% |
| p50 | 22344 ns | 22216 ns | -0.6% |
| p99 | 30088 ns | 33648 ns | +11.8% |
| p99.9 | 160832 ns | 194112 ns | +20.7% |
| max | 993766 ns | 882361 ns | -11.2% |

#### bench_order_rtt_dpdk — Response Latency (mock send -> app recv)

| Metric | Baseline (std::memcpy) | rte_memcpy | Delta |
|--------|----------------------|------------|-------|
| samples | 60000 | 60000 | - |
| min | 7044 ns | 7058 ns | +0.2% |
| p50 | 8044 ns | 7988 ns | -0.7% |
| p99 | 11236 ns | 11396 ns | +1.4% |
| p99.9 | 18728 ns | 16616 ns | -11.3% |
| max | 70839 ns | 57456 ns | -18.9% |

### 综合分析

60s 复测确认 10s 结果中的 tail latency 改善**不可复现**——属于运行间噪声。

| 指标 | 10s Delta | 60s Delta | 判定 |
|------|-----------|-----------|------|
| Market p50 | +1.2% | -5.0% | 噪声（方向不一致）|
| Market p99 | +3.8% | -18.2% | 噪声（方向不一致）|
| Order RTT p50 | -3.5% | -0.6% | 噪声（收敛到 ~0）|
| Order RTT p99 | -22.1% | +11.8% | **噪声**（方向翻转）|
| Response p99 | -29.4% | +1.4% | **噪声**（方向翻转）|

**总体结论**：rte_memcpy 在 ARM64 (NEON) 上对 E2E 延迟**无统计显著影响**。
所有 percentile 差异在 ±20% 范围内波动，且方向在两次运行间翻转——典型的
运行间变异。原因明确：reorder path 的 memcpy 在有序流量下几乎不触发，
即使触发，拷贝的 ≤1460 字节在 NEON 和 glibc memcpy 间无实质性能差异。

**建议**：保留 `--use_rte_memcpy` 编译开关（零维护成本），但默认关闭。
不建议在 ARM64 上默认启用。x86 (AVX/SSE) 环境可能有不同结论，待验证。
