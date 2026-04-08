# Plan: Benchmark 重设计

> 用双网卡背靠背 + 同进程 mock WS server 替代真实 Binance，从 9 个 bench 精简为 2 个场景 × 2 后端 = 4 个 target。

创建时间：2026-04-02
状态：已完成

---

## 定位与边界

**目标**：可重复、不依赖外部服务的 benchmark，公平对比 Socket vs DPDK 的端到端 Transport pipeline 延迟。

**In scope**：
- 2 个 benchmark 场景：订单 RTT、单连接多币种行情
- Mock WS server（同进程 kernel TCP，无 TLS）
- 双网卡背靠背拓扑 + 自动化 setup 脚本
- 统一指标：TSC 打点 → HdrHistogram

**Out of scope**：
- 多连接多币种（Reactor 模式）— 日后需要时再加
- TLS benchmark（TLS 层差异不是本次目标）
- 真实交易所连接的 benchmark（旧文件删除）

---

## 架构设计

### 网络拓扑

```
NIC-A (kernel, 10.0.0.1) ──物理线缆──> NIC-B (10.0.0.2)

Mock Server:   kernel TCP server on NIC-A:9999（同进程线程）
Socket Bench:  NIC-B (kernel driver) → connect(10.0.0.1:9999)
DPDK Bench:    NIC-B (DPDK PMD)     → TcpSession connect(10.0.0.1:9999)
```

Socket 和 DPDK bench 顺序执行，NIC-B 在两次之间通过 `dpdk-devbind.py` 切换驱动。物理路径完全相同，差异纯粹来自软件传输栈。

### 模块划分

| 模块 | 职责 | 文件 |
|------|------|------|
| Mock WS Server | 接受 WS 连接、推送行情、回应订单 | `benchmarks/mock/mock_ws_server.hpp` |
| Mock WS Handshake | HTTP 101 握手应答（复用 test 逻辑） | `benchmarks/mock/mock_ws_handshake.hpp` |
| Mock Data Generator | 生成 JSON 行情/订单回应，嵌入 TSC | `benchmarks/mock/mock_data_gen.hpp` |
| Bench 共享模板 | 场景逻辑（参数化 TcpImpl） | `benchmarks/bench_impl.hpp` |
| Bench Order RTT | 订单发送+回应 RTT | `benchmarks/bench_order_rtt.cpp` (socket) / `bench_order_rtt_dpdk.cpp` (DPDK) |
| Bench Market Single | 单连接多币种行情 | `benchmarks/bench_market_single.cpp` / `bench_market_single_dpdk.cpp` |
| Setup Script | NIC 配置 + 驱动切换 + bench 运行 | `scripts/bench_setup.sh` |
| Bench Common | TSC 工具、histogram 输出 | `benchmarks/bench_common.hpp`（保留，小修改） |

### 数据流

**行情 bench (bench_market_single)**：
```
Mock thread:  生成 JSON → TSC::now() 写入 "T" → WS frame encode → sendmsg(NIC-A)
              ↓ 物理线缆
NIC-B:        kernel recvmsg / DPDK rte_eth_rx_burst
Transport:    [TCP] → WS decode → on_message callback
App:          读取 "T" → TSC::now() - T → record_latency(histogram)
```

**订单 RTT bench (bench_order_rtt)**：
```
App:          TSC::now() → T_send → 构造订单 JSON → Transport.send()
Transport:    WS encode → [TCP] → sendmsg(NIC-B)
              ↓ 物理线缆
Mock thread:  recvmsg(NIC-A) → 解析订单 → 生成 ExecutionReport → TSC::now() 写入 "T"
              → WS frame encode → sendmsg(NIC-A)
              ↓ 物理线缆
NIC-B:        kernel recvmsg / DPDK rte_eth_rx_burst
Transport:    [TCP] → WS decode → on_message callback
App:          TSC::now() - T_send → record_rtt(histogram)
              读取 "T" → TSC::now() - T → record_response_latency(histogram)
```

---

## 接口设计

### Mock WS Server

```cpp
// benchmarks/mock/mock_ws_server.hpp

struct MockServerConfig {
    std::string bind_ip = "10.0.0.1";      // NIC-A IP
    uint16_t port = 9999;
    std::vector<std::string> symbols;       // 推送币种
    std::chrono::microseconds tick_interval{100}; // 行情推送频率
    bool order_mode = false;                // true=接受订单并回应
};

/// 启动 mock WS server。阻塞直到 running=false。
/// 监听 bind_ip:port，接受 WS 连接（无 TLS），按配置推送行情或回应订单。
void run_mock_ws_server(const MockServerConfig& config,
                        std::atomic<bool>& running);
```

### Mock Data Generator

```cpp
// benchmarks/mock/mock_data_gen.hpp

/// 生成 mock bookTicker JSON（模拟 Binance 格式）
/// "T" 字段 = 传入的 tsc 纳秒时间戳
/// 返回写入 buf 的字节数
size_t generate_book_ticker(char* buf, size_t buf_size,
                            std::string_view symbol,
                            uint64_t tsc_ns);

/// 生成 mock ExecutionReport JSON（订单回应）
/// 不是 echo——是独立的回应结构
size_t generate_execution_report(char* buf, size_t buf_size,
                                 std::string_view symbol,
                                 std::string_view side,
                                 uint64_t tsc_ns);
```

### Mock WS Handshake

```cpp
// benchmarks/mock/mock_ws_handshake.hpp

/// 读取客户端 HTTP Upgrade 请求，发送 101 Switching Protocols 应答。
/// 返回 true = 握手成功，false = 失败。
bool handle_ws_upgrade(int client_fd);
```

### Bench 共享模板

```cpp
// benchmarks/bench_impl.hpp

struct BenchConfig {
    std::string server_ip = "10.0.0.1";
    uint16_t server_port = 9999;
    std::vector<std::string> symbols;
    std::chrono::seconds duration{10};
    std::chrono::microseconds order_interval{1000}; // 订单发送间隔
    int rx_cpu = -1;
    int tx_cpu = -1;
    int main_cpu = -1;
    // DPDK specific
    std::string local_ip;       // NIC-B IP (10.0.0.2)
    std::string gateway_ip;     // NIC-A IP (10.0.0.1)
    uint16_t dpdk_port = 0;
};

/// 单连接多币种行情接收 bench
template <TcpTransport TcpImpl>
void run_market_bench(/* TcpFactory */ auto make_transport,
                      const BenchConfig& cfg);

/// 订单 RTT bench
template <TcpTransport TcpImpl>
void run_order_rtt_bench(/* TcpFactory */ auto make_transport,
                         const BenchConfig& cfg);
```

### JSON 数据格式

**行情（bookTicker）**：
```json
{"s":"BTCUSDT","b":"50000.00","a":"50001.00","T":1234567890123456789}
```

**订单请求**（客户端 → mock server）：
```json
{"method":"order.place","symbol":"BTCUSDT","side":"BUY","price":"50000.00","quantity":"0.001","T_send":1234567890123456789}
```

**订单回应**（mock server → 客户端）：
```json
{"e":"executionReport","s":"BTCUSDT","S":"BUY","o":"LIMIT","X":"NEW","p":"50000.00","q":"0.001","T":1234567890123456789}
```

### 延迟测量

| 场景 | 指标 | 打点方式 |
|------|------|----------|
| 行情 | Pipeline Latency | mock `sendmsg()` 前 TSC → app `on_message` TSC |
| 订单 | Order RTT | app `send()` 前 TSC → app 收到回应 TSC |
| 订单 | Response Latency | mock 回应 `sendmsg()` 前 TSC → app 收到回应 TSC |

TSC 解析：JSON "T" 字段用 `bench_common.hpp` 中的固定偏移快速解析（不用通用 JSON parser）。

### 输出格式

```
=== bench_market_single (socket) ===
Pipeline Latency (mock send → app recv):
  min=1.2µs  p50=3.4µs  p99=8.7µs  p99.9=15.2µs  max=42.1µs
  count=100000  rate=10000 msg/s
  symbols: BTCUSDT, ETHUSDT, SOLUSDT

=== bench_order_rtt (socket) ===
Order RTT (send → response recv):
  min=5.1µs  p50=12.3µs  p99=28.7µs  p99.9=45.2µs  max=102.1µs
Response Latency (mock send → app recv):
  min=1.1µs  p50=3.2µs  p99=8.1µs  p99.9=14.8µs  max=38.5µs
  orders=5000  rate=1000 order/s
```

---

## 文件结构

### 新增

```
benchmarks/
├── mock/
│   ├── mock_ws_server.hpp          ← mock WS server（kernel TCP，同进程线程）
│   ├── mock_ws_handshake.hpp       ← WS 握手应答
│   └── mock_data_gen.hpp           ← JSON 行情/订单回应生成
├── bench_impl.hpp                  ← 共享场景模板
├── bench_order_rtt.cpp             ← 订单 RTT (socket)
├── bench_order_rtt_dpdk.cpp        ← 订单 RTT (DPDK)
├── bench_market_single.cpp         ← 单连接行情 (socket)
└── bench_market_single_dpdk.cpp    ← 单连接行情 (DPDK)

scripts/
└── bench_setup.sh                  ← NIC setup + driver switch + bench runner
```

### 删除

```
benchmarks/
├── bench_market.cpp                ← 替换为 bench_market_single
├── bench_market_multi.cpp          ← 不再需要
├── bench_market_pingpong.cpp       ← 替换为 bench_order_rtt
├── bench_pingpong.cpp              ← 替换为 bench_order_rtt
├── bench_market_dpdk.cpp           ← 替换为 bench_market_single_dpdk
├── bench_market_multi_dpdk.cpp     ← 不再需要
├── bench_market_persymbol_dpdk.cpp ← 不再需要（多连接 out of scope）
├── bench_market_pingpong_dpdk.cpp  ← 替换为 bench_order_rtt_dpdk
└── bench_pingpong_dpdk.cpp         ← 替换为 bench_order_rtt_dpdk
```

### 保留（修改）

```
benchmarks/
├── bench_common.hpp                ← 保留，加入 mock TSC 解析工具
```

---

## Setup 脚本设计

```bash
# scripts/bench_setup.sh
# 用法: sudo ./bench_setup.sh --nic-a eth0 --nic-b eth1 --duration 10 --symbols BTCUSDT,ETHUSDT

# Phase 1: 配置网络
#   ip addr add 10.0.0.1/24 dev $NIC_A
#   ip addr add 10.0.0.2/24 dev $NIC_B
#   ip link set $NIC_A up && ip link set $NIC_B up

# Phase 2: Socket benchmarks (NIC-B kernel driver)
#   ./build/.../bench_market_single --server-ip 10.0.0.1 --symbols $SYMBOLS ...
#   ./build/.../bench_order_rtt --server-ip 10.0.0.1 --symbols $SYMBOLS ...

# Phase 3: 切换 NIC-B 到 DPDK
#   ip addr del 10.0.0.2/24 dev $NIC_B
#   dpdk-devbind.py -b vfio-pci $NIC_B_PCI

# Phase 4: DPDK benchmarks
#   ./build/.../bench_market_single_dpdk [EAL args] -- --local-ip 10.0.0.2 ...
#   ./build/.../bench_order_rtt_dpdk [EAL args] -- --local-ip 10.0.0.2 ...

# Phase 5: 恢复 NIC-B 到 kernel
#   dpdk-devbind.py -b $ORIGINAL_DRIVER $NIC_B_PCI
#   ip addr add 10.0.0.2/24 dev $NIC_B
```

---

## 实施计划

### 阶段 1: Mock WS Server
- 交付物：`mock_ws_server.hpp`, `mock_ws_handshake.hpp`, `mock_data_gen.hpp`
- 验收标准：mock server 能独立运行，接受 WS 连接，推送行情 JSON（带 TSC），回应订单
- 推荐 skill：`/design auto`

### 阶段 2: Bench 共享模板 + Socket Bench
- 交付物：`bench_impl.hpp`, `bench_market_single.cpp`, `bench_order_rtt.cpp`
- 验收标准：socket bench 能连接 mock server，测量延迟，输出 HdrHistogram 结果
- 推荐 skill：`/design auto`

### 阶段 3: DPDK Bench
- 交付物：`bench_market_single_dpdk.cpp`, `bench_order_rtt_dpdk.cpp`
- 验收标准：DPDK bench 编译通过（不要求运行——需要 DPDK 硬件）
- 推荐 skill：`/design auto`

### 阶段 4: Setup 脚本 + 清理旧 Bench
- 交付物：`scripts/bench_setup.sh`，删除 9 个旧 bench 文件，更新 xmake.lua
- 验收标准：`bench_setup.sh --help` 输出用法，xmake build 通过
- 推荐 skill：`/script` + `/git`

---

## 关键决策记录

### D-1: 公平性方案——双网卡背靠背
- **问题**：如何在同一台机器上公平对比 Socket vs DPDK
- **选项**：A. loopback / B. tap vdev / C. 双网卡背靠背
- **决策**：C（双网卡背靠背，NIC-B 切换驱动）
- **理由**：物理路径完全相同，差异纯粹来自软件栈。是唯一真正公平的方案
- **验收标准**：Socket 和 DPDK bench 通过同一条物理线缆通信

### D-2: Mock Server 实现——同进程 kernel TCP
- **问题**：mock server 如何运行
- **选项**：A. 独立进程 / B. 同进程线程
- **决策**：B（同进程线程，kernel TCP server on NIC-A）
- **理由**：不需要 IPC，TSC 时钟域一致，最简部署
- **验收标准**：bench binary 启动后自动创建 mock 线程

### D-3: 无 TLS
- **问题**：mock server 是否支持 TLS
- **决策**：不支持。`use_tls=false`
- **理由**：消除 OpenSSL/aws-lc 差异，聚焦 TCP + WS + Transport pipeline
- **验收标准**：TransportConfig.use_tls = false

### D-4: 订单回应不是 echo
- **问题**：mock server 如何回应订单
- **决策**：生成独立的 ExecutionReport JSON（非 echo）
- **理由**：模拟真实交易所行为（解析订单 → 生成回应），更有意义
- **验收标准**：回应 JSON 结构与请求不同，含独立 "T" 时间戳

### D-5: 2 个场景（非 3 个）
- **问题**：需要几个 benchmark 场景
- **决策**：2 个（订单 RTT + 单连接多币种行情），不做多连接
- **理由**：用户明确决定不做多连接多币种
- **验收标准**：4 个 target（2 场景 × 2 后端）

### D-6: 编译时后端选择（6→4 target）
- **问题**：Socket 和 DPDK bench 是同一 binary 还是分开
- **决策**：分开（4 个 target），场景逻辑通过模板共享
- **理由**：DPDK 有编译依赖，不是所有环境都有
- **验收标准**：非 DPDK 环境只编译 socket bench target
