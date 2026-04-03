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
