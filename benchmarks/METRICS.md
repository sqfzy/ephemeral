# Benchmark Latency Metrics

Transport 层测量三个关键延迟指标，覆盖请求-响应的完整生命周期。

## 1. TX Latency: App 产生请求包 -> 请求包交给网卡

```
App enqueue ──> SPSC queue ──> TX drain ──> WS encode ──> TLS encrypt ──> flush to kernel/NIC
│                                                                                          │
└──────────────────────── tx_latency ──────────────────────────────────────────────────────┘
                  │                    │                                                    │
                  └── tx_queue_wait ──┘                                                    │
                                       └────────────── tx_encode ─────────────────────────┘
```

- **tx_latency**: 总 TX 延迟（enqueue -> flush 完成）
  - **tx_queue_wait**: SPSC queue 等待时间（enqueue -> TX 线程 drain）
  - **tx_encode**: WS 编码 + TLS 加密 + 系统调用 flush

**测量点**: 在 `send()` 调用时记录 TSC 时间戳，在 TX 线程 flush 完成后计算差值。

## 2. RX Latency: 响应包离开网卡 -> App 收到响应包

```
NIC/kernel recv ──> TLS decrypt ──> WS decode ──> deliver (queue push / on_message callback)
│                                                                                         │
└──────────────────────── rx_latency ────────────────────────────────────────────────────┘
│                                   │                                                     │
└────────── rx_decrypt ────────────┘                                                     │
                                     └──────────── rx_decode ───────────────────────────┘
```

- **rx_latency**: 总 RX 延迟（rx_burst 收到数据 -> 帧解码完成并投递）
  - **rx_decrypt**: TLS 解密（rx_burst -> 明文就绪）
  - **rx_decode**: WS 帧解析 + 投递（解密完成 -> enqueue/callback）

**测量点**: 在 `rx_burst()` 返回数据时记录 TSC 时间戳，在帧处理完成后计算差值。

## 3. RTT: App 产生请求包 -> App 收到响应包

```
App send(ping) ──> [TX pipeline] ──> NIC ──> network ──> server ──> network ──> NIC ──> [RX pipeline] ──> pong received
│                                                                                                                     │
└──────────────────────────────────────────── RTT ───────────────────────────────────────────────────────────────────┘
```

- **rtt**: 端到端往返时间（ping flush 时刻 -> pong rx_burst 时刻）
- 包含: TX flush + 网络传播 + 服务器处理 + 网络传播 + RX burst 到达
- 不包含: TX queue wait（从 flush 后开始计时）、RX 解密解码（到 burst 为止）

**测量点**: ping 帧 flush 时记录 TSC 时间戳，对应 pong 帧 rx_burst 到达时计算差值。

## 4. Feed Latency: Binance 服务器发送 -> App 收到

```
Binance server generates event (E timestamp)
  │
  ├──> server internal processing
  ├──> TCP/TLS send
  ├──> network propagation (server → AWS region)
  ├──> NIC/kernel recv (or DPDK rx_burst)
  ├──> TLS decrypt
  ├──> WS decode
  └──> deliver to App
       │
       └── App records local wall-clock time
           │
           delta = local_time - E
```

- **feed_latency**: 交易所事件产生时刻到 App 收到的端到端延迟
- 包含: 交易所内部处理 + 网络传播 + RX pipeline（decrypt + decode + deliver）
- 依赖: 本地时钟与交易所时钟的同步精度（NTP/PTP）

**测量方法**: Binance bookTicker JSON 中的 `E` 字段为事件时间（Unix ms）。App 在 `recv()` 回调中取 `clock_gettime(CLOCK_REALTIME)` 或 `std::chrono::system_clock::now()`，计算差值。

**注意事项**:
- 精度受限于 NTP 同步质量（通常 1-10ms 误差）。AWS EC2 使用 Amazon Time Sync (chrony)，精度约 ~1ms
- 不适合测量亚毫秒级优化效果（用 RX Latency 代替）
- 适合测量: 网络路径选择（region proximity）、整体系统健康度、异常检测

**典型值**（AWS ap-northeast-1，Binance fstream）:
- p50: 1-5 ms
- p99: 10-50 ms（受网络抖动和交易所负载影响）

## Benchmark 与指标的对应关系

| Benchmark | TX | RX | RTT | Feed | 说明 |
|-----------|:---:|:---:|:---:|:---:|------|
| `bench_pingpong` | Y | Y | Y | - | 无行情订阅，纯 ping/pong |
| `bench_pingpong_dpdk` | Y | Y | Y | - | DPDK 后端，同上 |
| `bench_market` | - | Y | - | Y | 单 symbol 行情流 |
| `bench_market_dpdk` | - | Y | - | Y | DPDK 后端，同上 |
| `bench_market_multi` | - | Y | - | Y | 多 symbol，支持 `--mode twophase` frame filter |
| `bench_market_multi_dpdk` | - | Y | - | Y | DPDK 后端，同上 |

## 时间源

所有延迟测量使用 TSC (Time Stamp Counter)，启动时校准 `ns_per_cycle`。
TSC 精度约 1ns（取决于 CPU 频率），避免了 `clock_gettime` 的系统调用开销。

需要编译时启用: `EPH_ENABLE_TIMESTAMPS=1`（xmake 中 bench targets 已默认启用）。

## 统计输出格式

每个指标以 `RttStats` 结构报告:

```
RttStats (N samples):
  min: X.Xus, p50: X.Xus, p99: X.Xus, p999: X.Xus, max: X.Xus, mean: X.Xus
```

使用 HdrHistogram 记录，支持 `rx_latency_histogram_snapshot()` 导出逐秒窗口数据。
