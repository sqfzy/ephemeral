# Plan: UDP Benchmark Suite (Micro + E2E Latency)

> 两部分：(1) eph-dpdk 内部 UDP 微基准 (bench_udp.cpp)；(2) Kernel vs DPDK UDP E2E 延迟对比 (benchmarks/latency/)。

创建时间：2026-04-07
状态：已确认

---

## 定位与边界

**目标**：
1. 为 eph-dpdk 的 UDP 操作建立微基准性能基线（与 TCP 对应操作 A/B 对比）
2. 量化 Kernel Socket UDP vs DPDK UDP 的端到端延迟差异

**用户**：需要量化 UDP 延迟特性的交易系统开发者。

**In scope**：
- **Part A — eph-dpdk 微基准** (`eph-dpdk/benchmarks/bench_udp.cpp`)：
  - UdpPacketTemplate::fill / build 延迟
  - udp_checksum vs tcp_checksum 对比
  - 分层 parse API（parse_ip_header / parse_tcp_from_ip / parse_udp_from_ip）
  - parse_packet vs 分层 API 开销对比
  - Reactor UDP dispatch 模拟
- **Part B — Kernel vs DPDK E2E** (`benchmarks/latency/`)：
  - UDP mock server（raw UDP echo，嵌入 TSC 时间戳）
  - Kernel UDP client（`sendto` + `recvfrom` + 延迟直方图）
  - DPDK UDP client（`UdpSender::send` + `parse_udp_packet` + 延迟直方图）
  - 集成到 `bench_latency.sh` 调度脚本

**Out of scope**：
- Multicast 接收路径基准（已有 bench_multicast.cpp）
- TCP E2E 延迟基准（已存在 bench_market/bench_order_rtt）
- 跨机器网络延迟（本轮只做 loopback/namespace 内延迟）

---

## Part A: eph-dpdk 微基准

### 文件：`eph-dpdk/benchmarks/bench_udp.cpp`

### 基准测试清单

```cpp
// UDP TX 热路径
BM_UdpHeaderFill/{32,64,128,512,1472}     // fill() 预分配 mbuf
BM_UdpHeaderBuild                          // build() init+fill 路径

// Checksum
BM_UdpChecksum/{64,128,256,512,1024}       // udp_checksum

// 分层 Parse API
BM_ParseIpHeader/{64,128,512,1472}         // L2+L3
BM_ParseTcpFromIp/{64,128,512,1472}        // L4 TCP from pre-parsed IP
BM_ParseUdpFromIp/{64,128,512,1472}        // L4 UDP from pre-parsed IP
BM_ParsePacketLayered/{64,128,512,1472}    // ip_header + tcp_from_ip（分层）
BM_ParsePacketDirect/{64,128,512,1472}     // parse_packet（直接）

// Reactor UDP dispatch
BM_ReactorUdpDispatchSim/{1,2,4,8}        // hash + scan + callback
```

### 技术方案

- FakeMbuf 模式（无 EAL 依赖）
- PayloadSizeArgs: `{32, 64, 128, 512, 1472}`
- 与 `bench_tcp_header.cpp` 中 `BM_TcpHeaderBuild`/`BM_TcpChecksum`/`BM_ReactorDispatchSim` 形成对比

---

## Part B: Kernel vs DPDK UDP E2E 延迟

### 架构

```
Mock UDP Server (host namespace, NIC-A)
  ├── 监听 UDP port
  ├── 收到报文 → 嵌入 TSC 时间戳 → 原样回复
  └── 统计 echo 计数

Kernel UDP Client (bench namespace, NIC-B)
  ├── socket(AF_INET, SOCK_DGRAM) + bind
  ├── sendto → recv → 从 payload 提取 TSC → 计算 RTT
  └── HdrHistogram 统计延迟分布

DPDK UDP Client (直接访问 NIC-B via PMD)
  ├── UdpSender::send → rte_eth_rx_burst + parse_udp_packet
  ├── 从 payload 提取 TSC → 计算 RTT
  └── HdrHistogram 统计延迟分布
```

### 新增文件

| 文件 | 说明 |
|------|------|
| `benchmarks/latency/bench_udp_echo_server.cpp` | UDP echo server（raw socket，嵌入 TSC） |
| `benchmarks/latency/bench_udp_rtt.cpp` | Kernel socket UDP RTT 客户端 |
| `benchmarks/latency/bench_udp_rtt_dpdk.cpp` | DPDK UDP RTT 客户端 |

### Mock UDP Echo Server

```cpp
// bench_udp_echo_server.cpp
// 简单 UDP echo：recvfrom → 在 payload 前 8 字节写入 server TSC → sendto
// 命令行参数：--bind-ip, --port, --cpu
// 协议格式：payload[0:8] = uint64_t TSC (little-endian)
```

- 无 WebSocket、无 framing——纯 raw UDP
- TSC 时间戳嵌入 payload 前 8 字节
- 收到后立即回复（echo），延迟取决于 OS/DPDK 栈

### Kernel UDP Client (`bench_udp_rtt.cpp`)

```cpp
// 核心循环：
//   uint64_t send_tsc = TSC::now();
//   memcpy(buf, &send_tsc, 8);       // 嵌入发送 TSC
//   sendto(sockfd, buf, msg_size, ...);
//   recvfrom(sockfd, buf, ...);
//   uint64_t recv_tsc = TSC::now();
//   uint64_t rtt_ns = TSC::to_ns(recv_tsc - send_tsc);
//   histogram.record(rtt_ns);
```

- 使用 `SO_BINDTODEVICE` 绑定到 NIC-B
- `setsockopt(SO_RCVTIMEO)` 防止 hang
- 命令行：`--server-ip`, `--port`, `--msg-size`, `--count`, `--poll-cpu`

### DPDK UDP Client (`bench_udp_rtt_dpdk.cpp`)

```cpp
// 核心循环：
//   uint64_t send_tsc = TSC::now();
//   memcpy(buf, &send_tsc, 8);
//   sender.send(buf, msg_size);       // UdpSender
//   // poll RX for reply
//   while (true) {
//       auto pkts = rte_eth_rx_burst(...);
//       for each pkt:
//           auto parsed = parse_udp_packet(pkt);
//           if (parsed && matches reply port) {
//               uint64_t recv_tsc = TSC::now();
//               rtt_ns = TSC::to_ns(recv_tsc - send_tsc);
//               histogram.record(rtt_ns);
//               break;
//           }
//   }
```

- 使用 `UdpSender` 发送 + raw `rte_eth_rx_burst` + `parse_udp_packet` 接收
- Platform 初始化：EAL + port + queue + mempool
- ARP 解析 gateway MAC
- 命令行与 kernel 版一致 + DPDK 特有参数 (`--local-ip`, `--gateway-ip`)

### bench_latency.sh 扩展

在现有 TCP 测试后新增 UDP 阶段：

```bash
# Phase 3: UDP RTT
log_phase 3 "UDP RTT (kernel socket)"
bench_udp_echo_server --bind-ip $SERVER_IP --port 9997 --cpu $MOCK_CPU &
ip netns exec bench_ns bench_udp_rtt --server-ip $SERVER_IP --port 9997 ...

log_phase 4 "UDP RTT (DPDK)"
bench_udp_rtt_dpdk --server-ip $SERVER_IP --port 9997 --local-ip ... --gateway-ip ...
```

### 输出格式

与现有 TCP bench 一致——HdrHistogram 百分位输出：

```
UDP RTT (kernel): p50=12.3us p99=45.6us p999=123.4us max=234.5us count=100000
UDP RTT (dpdk):   p50=2.1us  p99=5.3us  p999=12.1us  max=34.2us  count=100000
```

---

## 实施计划

### 阶段 1: eph-dpdk 微基准 (`bench_udp.cpp`)

**交付物**：`eph-dpdk/benchmarks/bench_udp.cpp`
**验收标准**：`xmake build bench_udp && xmake run bench_udp` 成功
**预估**：~300 行

### 阶段 2: UDP Echo Server

**交付物**：`benchmarks/latency/bench_udp_echo_server.cpp`
**验收标准**：`xmake build bench_udp_echo_server` 成功，可独立运行并 echo UDP 报文
**预估**：~150 行

### 阶段 3: Kernel UDP RTT Client

**交付物**：`benchmarks/latency/bench_udp_rtt.cpp`
**验收标准**：`xmake build bench_udp_rtt` 成功，可连接 echo server 并输出延迟直方图
**预估**：~200 行

### 阶段 4: DPDK UDP RTT Client

**交付物**：`benchmarks/latency/bench_udp_rtt_dpdk.cpp`
**验收标准**：`xmake build bench_udp_rtt_dpdk` 成功，可用 DPDK PMD 连接 echo server
**预估**：~250 行

### 阶段 5: bench_latency.sh 集成

**交付物**：`scripts/bench_latency.sh` 更新（新增 UDP phases）
**验收标准**：`--dry-run` 模式输出正确的 UDP 命令序列
**预估**：~50 行

---

## 关键决策记录

### D-1: UDP Echo 协议
- **问题**：mock server 用什么协议？
- **决策**：Raw UDP echo，payload 前 8 字节 = uint64_t TSC
- **理由**：最简单、最低开销、与 TCP WebSocket bench 形成公平对比（TCP bench 有 WS framing 开销，UDP bench 没有——这正是真实场景的差异）

### D-2: DPDK RX 方式
- **问题**：DPDK UDP client 怎么接收回复？
- **决策**：直接 `rte_eth_rx_burst` + `parse_udp_packet`（不用 Reactor）
- **理由**：单连接 RTT bench 不需要 Reactor 分发。直接 poll 是最低延迟路径。

### D-3: 微基准与 E2E 分开
- **问题**：微基准和 E2E 放同一个文件还是分开？
- **决策**：分开。微基准在 `eph-dpdk/benchmarks/`，E2E 在 `benchmarks/latency/`。
- **理由**：微基准无需 EAL（FakeMbuf），E2E 需要 EAL + 网络。生命周期和运行条件完全不同。
