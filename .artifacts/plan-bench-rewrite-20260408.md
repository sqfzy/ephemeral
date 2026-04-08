# Plan: benchmarks/latency 完全重写

> 6 场景 × kernel/DPDK × multi-payload 的干净延迟基准测试架构

创建时间：2026-04-08
状态：已确认
前置讨论：`.artifacts/discuss-20260408-bench-rewrite.md`

---

## 定位与边界

**目标**：为 eph-net / eph-dpdk 的 TCP/UDP/WebSocket 路径提供可复现的纳秒级延迟基准测试，支持 kernel vs DPDK A/B 对比和 multi-payload sweep。

**In scope**：
- 6 个场景的完整实现（见场景矩阵）
- kernel socket / DPDK 双 transport 路径
- Multi-payload size sweep（进程内循环）
- 进程内 warmup + HdrHistogram 测量
- spdlog 人类可读输出 + JSONL 机器可读输出
- bench_latency.sh 编排脚本
- xmake.lua table-driven target 生成

**Out of scope**：
- 吞吐量基准测试（本 plan 只覆盖延迟）
- 跨机器分布式 bench（TSC 同步限制，仅同机）
- TLS 路径 bench（所有场景 use_tls=false）
- 超 MTU payload（TCP ≤1460B, UDP ≤1472B）

---

## 架构设计

### 文件结构

```
benchmarks/latency/
├── bench_config.hpp           — BenchConfig + CLI 解析 + WS transport selector
├── bench_loop.hpp             — BenchTimer + LegStats + compute/print/JsonlWriter
├── mock/
│   ├── ws_server.hpp          — WebSocket mock (market/order/echo mode)
│   ├── ws_handshake.hpp       — RFC 6455 handshake (现有)
│   ├── data_gen.hpp           — JSON payload generator (现有)
│   ├── tcp_echo_server.hpp    — Raw TCP echo server (fixed-size framing)
│   ├── udp_echo_server.hpp    — Raw UDP echo server (从 .cpp 提取为 header-only)
│   └── udp_relay_server.hpp   — UDP relay (recv on port A → forward to port B)
├── bench_tcp_echo.cpp         — 场景 1: Raw TCP echo
├── bench_udp_echo.cpp         — 场景 2: Raw UDP echo
├── bench_ws_echo.cpp          — 场景 3: WS echo
├── bench_market_rx.cpp        — 场景 4: WS market data RX
├── bench_order_rtt.cpp        — 场景 5: WS order RTT
├── bench_udp_relay.cpp        — 场景 6: UDP relay
└── bench_latency.sh           — 编排脚本
```

### 场景分类矩阵

| # | 场景 | Transport 层 | Mock 类型 | Payload 矩阵 | 测量指标 |
|---|------|-------------|-----------|-------------|---------|
| 1 | tcp_echo | raw socket / DPDK raw | tcp_echo_server | 64,128,256,512,1024,1460B | RTT,TX,RX,Server |
| 2 | udp_echo | raw socket / DPDK UdpSender | udp_echo_server | 64,128,512,1024,1472B | RTT,TX,RX,Server |
| 3 | ws_echo | DirectTransport+WsFramer | ws_server(echo mode) | 64,128,256,512,1024B | RTT,TX,RX,Server |
| 4 | market_rx | DirectTransport+WsFramer | ws_server(market mode) | 固定 JSON (~80B) | Pipeline latency (1-leg, 其余 samples=0) |
| 5 | order_rtt | DirectTransport+WsFramer | ws_server(order mode) | 固定 JSON (~120B) | RTT,TX,RX,Server |
| 6 | udp_relay | raw socket / DPDK UdpSender | udp_relay_server | 64,128,512,1024,1472B | RTT,TX,RX,Relay |

### 场景二分类

- **Raw 场景**（#1 tcp_echo, #2 udp_echo, #6 udp_relay）：直接操作 socket/DPDK API，不经过 DirectTransport。目的：测最小路径延迟。
- **WS 场景**（#3 ws_echo, #4 market_rx, #5 order_rtt）：使用 `DirectTransport<BenchTcpImpl, WsFramer, MaxPayload>`。目的：测 WS framing + transport 路径延迟。

### Transport selector

```cpp
// bench_config.hpp
#if defined(EPH_USE_DPDK)
#include "eph/dpdk/tcp.hpp"
using BenchTcpImpl = eph::dpdk::TcpSession<>;
#else
#include "eph/net/socket_transport.hpp"
using BenchTcpImpl = eph::net::SocketTransport;
#endif
```

仅 WS 场景使用。Raw 场景直接 `#ifdef EPH_USE_DPDK` 选择 socket vs DPDK API。

### 场景 6 UDP relay 拓扑

```
Client (bench) ──UDP──▶ Relay (in-process thread, port A)
                              │
                              ▼ recv → stamp T_recv → stamp T_send → sendto
                              │
Client (bench) ◀──UDP── Relay (forward to port B)
```

单进程。Client 从 port C 发到 relay port A，relay 转发到 client port B。
Client 在 port B 收回复。与 udp_echo 的区别：relay 是独立线程，有自己的 recv+send 路径。

### Mock server 生命周期

所有 mock server 共享统一接口：

```cpp
// bench_loop.hpp
struct MockHandle {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> running;
};
// 每个 mock 提供: MockHandle start_xxx(config, int cpu);
```

- **Kernel bench**：mock 作为外部进程（standalone binary，由 bench_latency.sh 管理）
- **DPDK bench**：mock 作为 in-process thread（由 bench binary 自行启动）

---

## 接口设计

### bench_config.hpp

```cpp
namespace bench {

struct BenchConfig {
    // 通用
    std::string server_ip;                              // required
    uint16_t server_port = 9999;
    std::vector<size_t> payload_sizes;                  // multi-payload sweep
    std::chrono::seconds warmup{2};
    std::chrono::seconds duration{10};
    int poll_cpu = 2;
    int mock_cpu = 4;
    std::string output_path;                            // JSONL, 空=不输出

    // WS-specific (场景 3/4/5)
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    std::chrono::microseconds tick_interval{100};
    std::chrono::microseconds order_interval{1000};

    // DPDK-specific (仅 EPH_USE_DPDK 构建)
    std::string local_ip;
    std::string gateway_ip;
    uint16_t dpdk_port_id = 0;
};

/// 解析 CLI 参数。未识别的参数忽略（DPDK EAL 参数由 EalGuard 消费）。
BenchConfig parse_bench_config(int argc, char** argv);

/// 默认 payload sizes（场景特定，由各 main 设置）
inline const std::vector<size_t> kTcpPayloads  = {64, 128, 256, 512, 1024, 1460};
inline const std::vector<size_t> kUdpPayloads  = {64, 128, 512, 1024, 1472};
inline const std::vector<size_t> kWsPayloads   = {64, 128, 256, 512, 1024};

} // namespace bench
```

CLI 参数格式：
```
--server-ip IP --port PORT --payload-sizes 64,128,512 --warmup 2
--duration 10 --poll-cpu 2 --mock-cpu 4 --output .bench/results.jsonl
--symbols BTCUSDT,ETHUSDT --tick-interval 100 --order-interval 1000
--local-ip IP --gateway-ip IP --dpdk-port 0
```

### bench_loop.hpp

```cpp
namespace bench {

// ── BenchTimer ────────────────────────────────────────────────────────────

class BenchTimer {
public:
    void start(std::chrono::seconds warmup, std::chrono::seconds duration);
    [[nodiscard]] bool is_warmup() const noexcept;    // warmup 阶段返回 true
    [[nodiscard]] bool is_running() const noexcept;   // warmup+measure 结束后返回 false
    [[nodiscard]] std::chrono::seconds elapsed() const noexcept;
private:
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point warmup_end_;
    std::chrono::steady_clock::time_point measure_end_;
};

// ── Statistics ────────────────────────────────────────────────────────────

struct LegStats {
    double p50_us  = 0;
    double p99_us  = 0;
    double p999_us = 0;
    double max_us  = 0;
    uint64_t samples = 0;
};

struct BenchResult {
    LegStats rtt, tx, rx, srv;
};

/// Pure computation: extract percentiles from histogram.
[[nodiscard]] LegStats compute_stats(const eph::utils::HdrHistogram& h);

/// Side effect: spdlog::info output.
void print_stats(const char* label, const LegStats& s);

/// Print full 4-leg report header + stats.
void print_bench_result(const char* scenario_label, const BenchResult& r);

// ── JSONL Writer ──────────────────────────────────────────────────────────

class JsonlWriter {
public:
    explicit JsonlWriter(const std::string& path);  // 空 path = no-op
    ~JsonlWriter();

    /// Write one measurement record. transport = "kernel" | "dpdk".
    void write(std::string_view scenario, std::string_view transport,
               size_t payload_size, const BenchResult& result);

private:
    FILE* file_ = nullptr;
};

// ── Mock Handle ───────────────────────────────────────────────────────────

struct MockHandle {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> running = std::make_shared<std::atomic<bool>>(true);
};

inline void stop_mock(MockHandle& h) {
    h.running->store(false, std::memory_order_release);
    if (h.thread.joinable()) h.thread.join();
}

// ── Signal handling ───────────────────────────────────────────────────────

inline std::atomic<bool> g_running{true};
inline void install_signal_handlers();  // SIGINT + SIGTERM → g_running = false

// ── CPU pinning ───────────────────────────────────────────────────────────

inline void pin_or_die(int cpu, const char* name);

} // namespace bench
```

### JSONL 输出格式

每条记录一行：
```json
{"scenario":"udp_echo","transport":"dpdk","payload":64,"leg":"rtt","p50_us":1.2,"p99_us":2.1,"p999_us":3.5,"max_us":15.0,"samples":100000}
{"scenario":"udp_echo","transport":"dpdk","payload":64,"leg":"tx","p50_us":0.6,"p99_us":1.0,"p999_us":1.8,"max_us":8.2,"samples":100000}
```

每个 BenchResult 产出 4 行（rtt/tx/rx/srv），samples=0 的 leg 跳过不输出。

### Mock server 接口

```cpp
// mock/tcp_echo_server.hpp
namespace bench::mock {
struct TcpEchoConfig {
    std::string bind_ip;
    uint16_t port = 9998;
    size_t msg_size = 64;       // 固定消息大小
};
MockHandle start_tcp_echo(const TcpEchoConfig& cfg, int cpu);
}

// mock/udp_echo_server.hpp
namespace bench::mock {
struct UdpEchoConfig {
    std::string bind_ip;
    uint16_t port = 9997;
};
MockHandle start_udp_echo(const UdpEchoConfig& cfg, int cpu);
}

// mock/udp_relay_server.hpp
namespace bench::mock {
struct UdpRelayConfig {
    std::string bind_ip;
    uint16_t listen_port = 9996;     // recv from client
    std::string forward_ip;
    uint16_t forward_port = 9995;    // send to client
};
MockHandle start_udp_relay(const UdpRelayConfig& cfg, int cpu);
}

// mock/ws_server.hpp — 扩展现有
namespace bench::mock {
struct MockServerConfig {
    // ... 现有字段 ...
    bool echo_mode = false;   // 新增：收到消息后原样回发 + TSC 时间戳
};
}
```

### TSC 时间戳协议

**Raw 场景（TCP/UDP echo, relay）**——binary 格式：
```
[0:8]   client_send_tsc    — client 写入
[8:16]  server_recv_tsc    — server/relay 写入（recvfrom 后立即）
[16:24] server_send_tsc    — server/relay 写入（sendto 前立即）
[24:N]  padding            — 填充到 payload_size
```
最小 payload_size = 24 bytes。

**WS 场景**——JSON 格式（与现有兼容）：
```json
{"T": <tsc>, "T_recv": <tsc>, ...}
```

### 典型场景 main 结构（以 bench_udp_echo.cpp 为例）

```cpp
#include "bench_config.hpp"
#include "bench_loop.hpp"
#include "mock/udp_echo_server.hpp"

int main(int argc, char** argv) {
    bench::install_signal_handlers();

    #if defined(EPH_USE_DPDK)
    auto eal = eph::dpdk::EalGuard::init(argc, argv);
    // ... split EAL args ...
    #endif

    auto cfg = bench::parse_bench_config(argc, argv);
    if (cfg.payload_sizes.empty()) cfg.payload_sizes = bench::kUdpPayloads;

    // DPDK: in-process mock; kernel: assume external mock
    #if defined(EPH_USE_DPDK)
    auto mock = bench::mock::start_udp_echo({cfg.server_ip, cfg.server_port}, cfg.mock_cpu);
    #endif

    bench::JsonlWriter jsonl(cfg.output_path);
    constexpr auto transport_name = 
        #if defined(EPH_USE_DPDK)
        "dpdk";
        #else
        "kernel";
        #endif

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");

    for (size_t payload : cfg.payload_sizes) {
        // Setup socket/DPDK sender...
        HdrHistogram rtt{10, 1'000'000'000ULL, 3};
        HdrHistogram tx{10, 1'000'000'000ULL, 3};
        HdrHistogram rx{10, 1'000'000'000ULL, 3};
        HdrHistogram srv{10, 1'000'000'000ULL, 3};

        BenchTimer timer;
        timer.start(cfg.warmup, cfg.duration);

        while (timer.is_running() && g_running.load()) {
            // sendto / recvfrom / record...
            if (!timer.is_warmup()) {
                rtt.record(...);
                // ...
            }
        }

        auto result = BenchResult{
            compute_stats(rtt), compute_stats(tx),
            compute_stats(rx), compute_stats(srv)
        };
        print_bench_result("UDP Echo", result);
        jsonl.write("udp_echo", transport_name, payload, result);
    }

    #if defined(EPH_USE_DPDK)
    bench::stop_mock(mock);
    #endif
}
```

---

## 编码规范

| 维度 | 规范 |
|------|------|
| 命名 | 场景文件: `bench_<scenario>.cpp`；mock: `<protocol>_<role>_server.hpp`；framework: `bench_<purpose>.hpp` |
| 错误处理 | `spdlog::error() + return 1`（bench binary，不用 exception） |
| 日志 | spdlog INFO: bench 进度 + 结果；DEBUG: 每 10000 tick 进度；ERROR: 初始化失败 |
| Histogram | 统一 `{10, 1'000'000'000ULL, 3}` 构造参数（10ns~1s，3 位有效数字） |
| TSC | 全部用 `eph::utils::TSC::now()` + `TSC::to_ns()`，不用 `chrono` 做延迟测量 |
| CPU pin | poll thread pin `poll_cpu`(default 2)，mock thread pin `mock_cpu`(default 4) |
| 注释 | 每个 .cpp 文件头: `@file` + 场景描述 + usage 示例 |

---

## xmake.lua

```lua
-- benchmarks/latency targets (table-driven)
local bench_scenarios = {
    {name="tcp_echo",    kernel_deps={"eph-net"},   dpdk_deps={"eph-dpdk","eph-net"}},
    {name="udp_echo",    kernel_deps={"eph-utils"}, dpdk_deps={"eph-dpdk","eph-utils"}},
    {name="ws_echo",     kernel_deps={"eph-net"},   dpdk_deps={"eph-dpdk","eph-net"}},
    {name="market_rx",   kernel_deps={"eph-net"},   dpdk_deps={"eph-dpdk","eph-net"}},
    {name="order_rtt",   kernel_deps={"eph-net"},   dpdk_deps={"eph-dpdk","eph-net"}},
    {name="udp_relay",   kernel_deps={"eph-utils"}, dpdk_deps={"eph-dpdk","eph-utils"}},
}

local bench_common_flags = {"-fno-omit-frame-pointer", "-march=native"}

for _, s in ipairs(bench_scenarios) do
    -- Kernel variant
    target("bench_" .. s.name)
        set_kind("binary")
        set_group("benchmarks")
        set_default(false)
        add_files("benchmarks/latency/bench_" .. s.name .. ".cpp")
        add_includedirs("benchmarks", "benchmarks/latency")
        for _, d in ipairs(s.kernel_deps) do add_deps(d) end
        add_packages("spdlog")
        add_defines("EPH_ENABLE_TIMESTAMPS=1")
        add_cxflags(table.unpack(bench_common_flags))
        set_symbols("debug")

    -- DPDK variant
    target("bench_" .. s.name .. "_dpdk")
        set_kind("binary")
        set_group("benchmarks")
        set_default(false)
        add_files("benchmarks/latency/bench_" .. s.name .. ".cpp")  -- 同一源文件
        add_includedirs("benchmarks", "benchmarks/latency")
        for _, d in ipairs(s.dpdk_deps) do add_deps(d) end
        add_packages("spdlog")
        add_defines("EPH_ENABLE_TIMESTAMPS=1", "EPH_USE_DPDK=1")
        add_cxflags(table.unpack(bench_common_flags))
        set_symbols("debug")
        apply_dpdk_pmd_linkgroups()
end

-- Standalone mock servers (kernel socket, for bench_latency.sh)
target("bench_mock_server")
    -- ... 现有 WS mock server binary (保留) ...

target("bench_tcp_echo_server")
    set_kind("binary")
    set_group("benchmarks")
    set_default(false)
    add_files("benchmarks/latency/mock/tcp_echo_server_main.cpp")  -- thin main wrapper
    add_includedirs("benchmarks", "benchmarks/latency")
    add_deps("eph-utils")
    add_packages("spdlog")
    set_symbols("debug")

target("bench_udp_echo_server")
    -- ... 保留，源文件改为 thin main wrapper ...
```

---

## bench_latency.sh 结构

```bash
#!/usr/bin/env bash
set -euo pipefail

# ─── 默认值 ───
SCENARIOS="tcp_echo udp_echo ws_echo market_rx order_rtt udp_relay"
TRANSPORTS="kernel dpdk"
DURATION=10
WARMUP=2
OUTPUT_DIR=".bench"
POLL_CPU=2
MOCK_CPU=4

# ─── 函数 ───

check_prereqs() {
    # hugepages (DPDK), cpu isolation, NIC status
}

record_env_metadata() {
    # uname -r, lscpu, ethtool -i, DPDK version → $OUTPUT_DIR/env_$ts.txt
}

payload_sizes() {
    case "$1" in
        tcp_echo)   echo "64 128 256 512 1024 1460" ;;
        udp_echo|udp_relay) echo "64 128 512 1024 1472" ;;
        ws_echo)    echo "64 128 256 512 1024" ;;
        market_rx|order_rtt) echo "0" ;;  # 固定 payload，0 = 不传 --payload-sizes
    esac
}

mock_for_scenario() {
    case "$1" in
        tcp_echo)   echo "bench_tcp_echo_server" ;;
        udp_echo)   echo "bench_udp_echo_server" ;;
        ws_echo|market_rx|order_rtt) echo "bench_mock_server" ;;
        udp_relay)  echo "bench_udp_relay_server" ;;  # 或不需要外部 mock
    esac
}

setup_mock() {
    local scenario=$1
    local mock_bin=$(mock_for_scenario "$scenario")
    # 启动 mock server，记录 PID
}

teardown_mock() {
    # kill mock server PID
}

setup_netns() {
    # kernel bench: 创建 netns 隔离，避免 loopback
}

teardown_netns() {
    # 清理 netns
}

run_bench() {
    local scenario=$1 transport=$2 payload=$3
    local bin="bench_${scenario}"
    [[ "$transport" == "dpdk" ]] && bin="${bin}_dpdk"
    
    local args="--server-ip $SERVER_IP --duration $DURATION --warmup $WARMUP"
    args+=" --output $OUTPUT_DIR/latency_results.jsonl"
    [[ "$payload" != "0" ]] && args+=" --payload-sizes $payload"
    
    if [[ "$transport" == "kernel" ]]; then
        ip netns exec bench_ns ./$bin $args
    else
        ./$bin $EAL_ARGS -- $args --local-ip $LOCAL_IP --gateway-ip $GW_IP
    fi
}

generate_summary() {
    # 读 JSONL，用 jq 生成对比表
}

# ─── 主流程 ───

parse_args "$@"
check_prereqs
record_env_metadata

for scenario in $SCENARIOS; do
    [[ "$TRANSPORTS" == *"kernel"* ]] && setup_netns
    setup_mock "$scenario"
    
    for transport in $TRANSPORTS; do
        for payload in $(payload_sizes "$scenario"); do
            run_bench "$scenario" "$transport" "$payload"
        done
    done
    
    teardown_mock "$scenario"
    [[ "$TRANSPORTS" == *"kernel"* ]] && teardown_netns
done

generate_summary
```

---

## 实施计划

> **Commit 策略**：每个阶段完成并通过验收后，执行 `/git` 提交。commit message 标注阶段编号。

### 阶段 1: Framework + UDP echo 贯通

**交付物**：
- `bench_config.hpp`：BenchConfig struct + `parse_bench_config()` + transport selector
- `bench_loop.hpp`：BenchTimer + LegStats + `compute_stats()` + `print_stats()` + `print_bench_result()` + JsonlWriter + MockHandle + signal handling + `pin_or_die()`
- `mock/udp_echo_server.hpp`：从现有 `bench_udp_echo_server.cpp` 提取为 header-only
- `bench_udp_echo.cpp`：完整实现，支持 kernel + DPDK 双编译
- xmake.lua：table-driven target 生成（先只含 udp_echo）
- 删除旧文件：`bench_udp_rtt.cpp`, `bench_udp_rtt_dpdk.cpp`, `bench_udp_echo_server.cpp`（被新代码替代）

**验收标准**：
- `xmake build bench_udp_echo` 和 `xmake build bench_udp_echo_dpdk` 均编译通过
- kernel 模式运行 multi-payload sweep（64,128,512,1024,1472B），spdlog 输出 4-leg stats
- JSONL 文件正确写入
- warmup 阶段的样本不出现在最终报告中

**推荐 skill**：`/design auto`

### 阶段 2: 剩余 Raw 场景 (tcp_echo + udp_relay)

**交付物**：
- `mock/tcp_echo_server.hpp`：fixed-size framing TCP echo server
- `mock/udp_relay_server.hpp`：UDP relay (recv → forward)
- `bench_tcp_echo.cpp`：Raw TCP echo，支持双编译
- `bench_udp_relay.cpp`：UDP relay，支持双编译
- `mock/tcp_echo_server_main.cpp`：standalone binary thin wrapper
- xmake.lua 表中新增 tcp_echo + udp_relay
- 删除旧文件：无（这两个场景是新增）

**验收标准**：
- tcp_echo kernel + DPDK 编译通过，multi-payload sweep 正确
- udp_relay kernel + DPDK 编译通过，relay 延迟 > echo 延迟（验证 relay 中间跳有效）
- JSONL 输出包含新场景数据

**推荐 skill**：`/design auto`

### 阶段 3: WS 场景 (ws_echo + market_rx + order_rtt)

**交付物**：
- `mock/ws_server.hpp`：扩展现有 mock_ws_server.hpp，新增 `echo_mode`
- `bench_ws_echo.cpp`：WS echo，使用 DirectTransport+WsFramer，支持双编译
- `bench_market_rx.cpp`：重构现有 bench_market.cpp，使用新 framework
- `bench_order_rtt.cpp`：重构现有 bench_order_rtt.cpp，使用新 framework
- xmake.lua 表中新增 ws_echo + market_rx + order_rtt
- 删除旧文件：`bench_market.cpp`, `bench_market_dpdk.cpp`, `bench_order_rtt.cpp`(旧), `bench_order_rtt_dpdk.cpp`, `bench_market_tx.cpp`, `bench_market_tx_dpdk.cpp`, `bench_mock_server.cpp`(旧), `bench_impl.hpp`(旧), `bench_common.hpp`(旧)
- 注意：现有 market_tx 场景被 ws_echo 替代（ws_echo 更通用，覆盖 TX 延迟测量）

**验收标准**：
- ws_echo kernel + DPDK 编译通过，multi-payload sweep 正确
- market_rx 和 order_rtt 结果与旧实现在同数量级（无性能回归）
- 所有 WS 场景 JSONL 输出正确
- mock/ws_server.hpp echo_mode 工作正常

**推荐 skill**：`/design auto`

### 阶段 4: bench_latency.sh 编排 + 清理

**交付物**：
- `bench_latency.sh`：完整编排脚本（netns、mock 管理、multi-payload sweep、环境元数据、结果汇总）
- xmake.lua 清理：删除所有旧 target 定义，只保留 table-driven 生成
- mock/ 旧文件清理：`mock_ws_server.hpp` → `ws_server.hpp` 重命名
- 确认所有旧文件已删除

**验收标准**：
- `bench_latency.sh --scenario all --transport kernel --duration 5` 全 6 场景通过
- `.bench/latency_results.jsonl` 包含所有场景数据
- `.bench/env_*.txt` 包含环境元数据
- `bench_latency.sh --scenario udp_echo --transport dpdk` 单场景执行正确
- xmake.lua bench 区域从 ~120 行缩减到 ~40 行

**推荐 skill**：`/design auto`

---

## 关键决策记录

### D-1: Multi-payload 进程内循环
- **问题**：每个 payload size 独立进程还是进程内循环？
- **选项**：A. 进程级隔离 / B. 进程内循环 + HdrHistogram::reset()
- **决策**：B
- **理由**：HdrHistogram 有 reset() 方法；DPDK EAL 初始化耗时 3-5s，N 个 payload 乘以这个开销不可接受
- **验收标准**：多 payload sweep 的各 size 结果互不影响（p50 偏差 <5%）

### D-2: Raw 场景绕过 DirectTransport
- **问题**：场景 1/2/6 是否使用 DirectTransport？
- **选项**：A. 统一用 DirectTransport（含 RawFramer） / B. 直接操作 socket/DPDK API
- **决策**：B
- **理由**：Raw 场景的目的是测最小路径延迟，DirectTransport 的 buffer 管理和 framing 层会污染测量
- **验收标准**：raw tcp_echo 延迟 < ws_echo 延迟（证明无 framing overhead）

### D-3: Raw TCP 固定大小 framing
- **问题**：Raw TCP echo 如何划定消息边界？
- **选项**：A. 4-byte length prefix / B. 固定大小（双方约定） / C. 无 framing
- **决策**：B
- **理由**：零 framing 开销。bench 环境下双方由同一 shell 脚本启动，--payload-size 参数一致，无错配风险
- **验收标准**：server 每次 recv 恰好 N bytes，不多不少

### D-4: BenchTimer 而非 run_bench() 框架
- **问题**：warmup/measure 生命周期由框架管理还是场景管理？
- **选项**：A. 框架提供 `run_bench(PollFn)` / B. 场景用 `BenchTimer::is_warmup()` 自行判断
- **决策**：B
- **理由**：Raw 和 WS 场景的 poll loop 结构完全不同，强行统一到一个 PollFn 签名是过度抽象。BenchTimer 是纯值类型，零开销，inline 内联
- **验收标准**：warmup 样本不出现在最终 histogram 中
