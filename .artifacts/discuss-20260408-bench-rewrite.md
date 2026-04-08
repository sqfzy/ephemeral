# Discussion Record

## Context
- 时间：2026-04-08
- 用户原始需求：完全重写 benchmarks/latency，目标是 6 个场景 × kernel/DPDK × 多 payload size 的干净架构
- 复杂度评估：高
- 讨论轮数：7 轮
- 参与角色：R2 极简主义者、R3 性能狂热者、R6 维护性倡导者、R14 架构师、R15 基准测试方法论专家（自定义）

## 内容摘要
核心争议围绕文件组织方案（独立 .cpp vs 源码共享 vs #ifdef）、warmup 位置（进程内 vs shell 层）、Raw 场景是否用 DirectTransport、echo server 是否合并。R2 提出极简方案后被 R3/R6 修正为 xmake 双编译（源码共享但产独立二进制），R14 的分层方案被简化为两个 .hpp 拆分，R15 强制加入进程内 warmup 和环境元数据记录。第 6 轮 R3 提出 Raw 场景绕过 DirectTransport 直接操作 socket/DPDK 获全员认同，成为场景分类的关键分水岭。最终全员收敛：6 .cpp × xmake 双编译 = 12 binary，扁平目录，JSONL 输出 + shell 编排。

---

## 最终方案

### 文件结构
```
benchmarks/latency/
├── bench_config.hpp           — BenchConfig + CLI 解析 + WS transport selector
├── bench_loop.hpp             — warmup→measure→report runner + histogram 工具
├── mock/
│   ├── ws_server.hpp          — WebSocket mock (market/order/echo mode)
│   ├── ws_handshake.hpp       — RFC 6455 handshake
│   ├── data_gen.hpp           — JSON payload generator
│   ├── tcp_echo_server.hpp    — Raw TCP echo server (length-prefix framing)
│   ├── udp_echo_server.hpp    — Raw UDP echo server (从现有 .cpp 提取)
│   └── udp_relay_server.hpp   — UDP relay (recv → forward)
├── bench_tcp_echo.cpp         — 场景 1: Raw TCP echo
├── bench_udp_echo.cpp         — 场景 2: Raw UDP echo
├── bench_ws_echo.cpp          — 场景 3: WS echo
├── bench_market_rx.cpp        — 场景 4: WS market data RX
├── bench_order_rtt.cpp        — 场景 5: WS order RTT
├── bench_udp_relay.cpp        — 场景 6: UDP relay
└── bench_latency.sh           — 编排脚本
```

### 场景分类矩阵

| 场景 | Transport 层 | Mock 类型 | Payload 矩阵 | 测量指标 |
|------|-------------|-----------|-------------|---------|
| tcp_echo | raw socket / DPDK raw | tcp_echo_server | 64,128,256,512,1024,1460B | RTT,TX,RX,Server |
| udp_echo | raw socket / DPDK UdpSender | udp_echo_server | 64,128,512,1024,1472B | RTT,TX,RX,Server |
| ws_echo | DirectTransport+WsFramer | ws_server(echo) | 64,128,256,512,1024B | RTT,TX,RX,Server |
| market_rx | DirectTransport+WsFramer | ws_server(market) | 固定 JSON | Pipeline latency |
| order_rtt | DirectTransport+WsFramer | ws_server(order) | 固定 JSON | RTT,TX,RX,Server |
| udp_relay | raw socket / DPDK UdpSender | udp_relay_server | 64,128,512,1024,1472B | RTT,TX,RX,Relay |

### 关键设计决策

1. **xmake 双编译**：每场景一个 .cpp，xmake for 循环 + EPH_USE_DPDK define 编译两次，产独立 binary
2. **场景二分类**：Raw（tcp_echo, udp_echo, udp_relay）绕过 DirectTransport；WS-based（ws_echo, market_rx, order_rtt）用 DirectTransport+WsFramer
3. **进程内 warmup**：bench_loop.hpp 提供 warmup→measure→report 生命周期，默认 2-3s warmup
4. **Multi-payload 进程级隔离**：shell 循环驱动，每个 payload size 独立进程
5. **输出双通道**：spdlog stdout（人类可读）+ JSONL 文件（机器可读）
6. **bench_latency.sh**：管理 netns（kernel bench）、mock 启停、hugepage 检查、环境元数据、结果汇总

### xmake.lua 驱动表
```lua
local bench_scenarios = {
    {name="tcp_echo",    deps={"eph-net"},             dpdk_deps={"eph-dpdk"}},
    {name="udp_echo",    deps={"eph-utils"},           dpdk_deps={"eph-dpdk"}},
    {name="ws_echo",     deps={"eph-net"},             dpdk_deps={"eph-net","eph-dpdk"}},
    {name="market_rx",   deps={"eph-net"},             dpdk_deps={"eph-net","eph-dpdk"}},
    {name="order_rtt",   deps={"eph-net"},             dpdk_deps={"eph-net","eph-dpdk"}},
    {name="udp_relay",   deps={"eph-utils"},           dpdk_deps={"eph-dpdk"}},
}
```

### Warmup + 测量 runner
```cpp
// bench_loop.hpp
template <typename PollFn>
BenchResult run_bench(PollFn&& poll_fn,
                      std::chrono::seconds warmup,     // 默认 2-3s
                      std::chrono::seconds duration,   // 默认 10s
                      const char* label);
// PollFn 签名: void(HdrHistogram& hist) — 场景自行 record
```

### bench_latency.sh 结构
```bash
check_prereqs          # hugepages, cpu isolation, NIC status
parse_args "$@"        # --scenario, --transport, --duration, --output-dir
record_env_metadata    # uname, ethtool, lscpu, dpdk version
for scenario; do
  setup_mock $scenario
  for transport; do
    for payload in $(payload_sizes $scenario); do
      run_bench $scenario $transport $payload
    done
  done
  teardown_mock $scenario
done
generate_summary       # read JSONL, produce comparison table
```

### 已解决的分歧

| 分歧点 | 解决方式 | 关键论据 |
|--------|----------|----------|
| 文件组织 (a)/(b)/(c) | xmake 双编译（源码共享、二进制独立） | R3: 二进制隔离保证测量独立性；R6: 源码共享减少维护负担 |
| Warmup 位置 | 进程内（bench_loop.hpp） | R15: DPDK 重启成本太高，shell 层 warmup 不可行 |
| Multi-payload 策略 | 进程级隔离（shell 循环） | R3: DPDK mbuf pool 与 payload 相关 |
| Raw 场景是否用 DirectTransport | 不用 | R3: 测最小路径延迟不应引入中间层 |
| Echo server 合并 | 分开 | R2: TCP 流式 vs UDP 数据报是不同抽象 |
| Payload 上限 | TCP 1460B, UDP 1472B | R2/R3: latency bench 不测跨 segment |
| 场景 6 拓扑 | 简化（client → relay → client 单进程） | R2: 三进程增加 scheduling jitter |
| Output 格式 | spdlog stdout + JSONL file | R3: 避免 stdout/stderr 混乱 |

### 未解决的权衡（需用户决策）

| 冲突 | 选项 A | 选项 B | 建议 |
|------|--------|--------|------|
| ws_server echo mode | 复用 ws_server 加 echo mode | WS echo 场景自行实现简单 echo | 先选简单方案（自行实现） |
| bench_config.hpp 拆分 | 单文件 | 拆为 config + cli + selector | 6 场景规模选单文件 |
| DPDK mock 管理 | 全部 in-process | 提供 --external-mock flag | 默认 in-process |
