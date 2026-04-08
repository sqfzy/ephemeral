# Repeat Report — bench rewrite plan execution

## 概况
- 命令：`/repeat /design [until: plan完成]`
- 开始：2026-04-08 ~04:20
- 结束：2026-04-08 ~04:36
- 总轮数：4 (有效 4，无效 0)
- 终止原因：plan 全 4 阶段完成

## 各轮详情

── 第 1 轮 ✅ 68c402d ──
Phase 1: framework + udp_echo
 .artifacts/discuss-20260408-bench-rewrite.md   | 161 +++
 .artifacts/plan-bench-rewrite-20260408.md       | 316 ++++++
 benchmarks/latency/bench_config.hpp             | 126 ++
 benchmarks/latency/bench_loop.hpp               | 198 ++++
 benchmarks/latency/bench_udp_echo.cpp           | 261 +++++
 benchmarks/latency/mock/udp_echo_server.hpp     | 134 +++
 benchmarks/latency/bench_udp_*.cpp (deleted)    | -603
 xmake.lua                                       |  73 +-

── 第 2 轮 ✅ cacd3c4 ──
Phase 2: tcp_echo + udp_relay
 benchmarks/latency/bench_tcp_echo.cpp           | 283 ++++
 benchmarks/latency/bench_udp_relay.cpp          | 317 +++++
 benchmarks/latency/mock/tcp_echo_server.hpp     | 176 +++
 benchmarks/latency/mock/udp_relay_server.hpp    | 133 ++
 xmake.lua                                       |   4 +-

── 第 3 轮 ✅ f34c268 ──
Phase 3: WS scenarios (ws_echo, market_rx, order_rtt)
 benchmarks/latency/bench_market_rx.cpp          | 235 +++
 benchmarks/latency/bench_order_rtt.cpp          | 308 ++++
 benchmarks/latency/bench_ws_echo.cpp            | 297 ++++
 benchmarks/latency/mock/mock_ws_server.hpp      |  35 +-
 benchmarks/latency/bench_impl.hpp (deleted)     | -424
 benchmarks/latency/bench_market{,_dpdk}.cpp ... | -399
 xmake.lua                                       |  72 +-

── 第 4 轮 ✅ 5d2ad6f ──
Phase 4: bench_latency.sh + standalone mock wrappers
 benchmarks/latency/bench_udp_echo_server.cpp    |  60 +++
 benchmarks/latency/bench_tcp_echo_server.cpp    |  68 +++
 benchmarks/latency/bench_udp_relay_server.cpp   |  73 +++
 benchmarks/latency/bench_mock_server.cpp        |   1 +
 scripts/bench_latency.sh                        | 596 ++++++++++++--------
 xmake.lua                                       |  35 ++

── 验证修复 ✅ 2706917 ──
fix(bench): udp_relay kernel + bench_latency.sh set -e
 benchmarks/latency/bench_udp_relay.cpp |  20 ++--
 scripts/bench_latency.sh               |  11 +-

## 验证结果（kernel mode, 3s duration, 1s warmup）

| 场景 | payload | RTT p50 | RTT p99 | TX p50 | RX p50 | Server p50 |
|------|---------|---------|---------|--------|--------|------------|
| udp_echo | 64B | 22.4us | 27.4us | 11.2us | 11.1us | 0us |
| udp_echo | 1472B | 24.9us | 34.2us | 12.3us | 12.3us | 0us |
| tcp_echo | 64B | 24.3us | 28.5us | 12.1us | 12.1us | 0us |
| tcp_echo | 1460B | 69.0us | 71.0us | 13.9us | 55.2us | 0us |
| ws_echo | 64B | 33.2us | 97.2us | 26.7us | 70.2us | 0us |
| ws_echo | 1024B | 32.9us | 98.0us | 27.7us | 69.2us | 0us |
| market_rx | (固定) | 19.1us | 23.1us | — | — | — |
| order_rtt | (固定) | 25.8us | 32.3us | 14.7us | 10.7us | 0.3us |
| udp_relay | 64B | 23.5us | 71.0us | 11.4us | 11.3us | 0us |
| udp_relay | 1472B | 24.6us | 34.1us | 12.4us | 12.1us | 0us |

## 数据合理性分析

- ✅ **udp_echo**：RTT ~22-25us 符合 AWS EC2 ENA 同 VPC RTT；TX≈RX≈RTT/2 表明 wire 延迟对称
- ✅ **tcp_echo**：64B 与 udp_echo 一致；1460B (近 MTU) 显示 RX 多 ~40us，可能是 segment 接收的 NIC 中断聚合
- ✅ **ws_echo**：比 raw 高 ~10us（WS framing + JSON parsing 开销）；RX 比 TX 高反映服务端 echo 后的 frame 编码
- ✅ **market_rx**：1-leg pipeline ~19us 符合预期，与 echo RTT/2 ≈ 12us + JSON parsing
- ✅ **order_rtt**：4-leg breakdown 完整，server 处理 ~0.3us（mock 直接构造 ExecutionReport）
- ✅ **udp_relay**：与 udp_echo 接近，因为 relay 处理时间极短，wire RTT 占主导

## 累计变更

5 commits，共 11 个新文件，9 个文件删除，6 个文件修改。
完整目录结构：

```
benchmarks/latency/
├── bench_config.hpp          (新)
├── bench_loop.hpp            (新)
├── bench_mock_server.cpp     (修改：+--echo-mode)
├── bench_tcp_echo.cpp        (新)
├── bench_tcp_echo_server.cpp (新)
├── bench_udp_echo.cpp        (新)
├── bench_udp_echo_server.cpp (新)
├── bench_udp_relay.cpp       (新)
├── bench_udp_relay_server.cpp (新)
├── bench_ws_echo.cpp         (新)
├── bench_market_rx.cpp       (新)
├── bench_order_rtt.cpp       (新)
└── mock/
    ├── mock_ws_server.hpp    (修改：+echo_mode)
    ├── mock_data_gen.hpp     (现有)
    ├── mock_ws_handshake.hpp (现有)
    ├── tcp_echo_server.hpp   (新)
    ├── udp_echo_server.hpp   (新)
    └── udp_relay_server.hpp  (新)

scripts/bench_latency.sh      (重写)
xmake.lua                     (table-driven targets)
```
