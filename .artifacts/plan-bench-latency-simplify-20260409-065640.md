# Plan: benchmarks/latency 用户交互极简化

> 每个场景一个 self-contained binary（mock + client + main 合为单文件）+ 一个 `scripts/lat` wrapper 透明管理 NIC 状态。用户永远只敲一条命令 `sudo ./scripts/lat <scenario> [--dpdk]`。

创建时间：2026-04-09
状态：已完成（2026-04-09）
完成记录：.artifacts/bench-simplify-e2e-20260409.md

---

## 定位与边界

**目标**：把 latency bench 的用户交互降到**每场景一条命令**。配置写在 `bench.conf` 里一次；之后所有测试都是 `sudo ./scripts/lat <scenario> [--dpdk]`。NIC-B 的状态切换（kernel netns ↔ DPDK vfio-pci）由 wrapper 透明管理，用户不需要知道 `ip netns`、`bench_ns`、`dpdk-setup.sh` 这些概念。

**用户**：项目维护者自己 + 任何想跑延迟对比的人。

**In scope**：
- 合并每个场景的 mock + client + main 到**单个 cpp**（plus exchange 共享 mock 用 hpp）
- `scripts/lat` wrapper：检测 NIC-B 状态、必要时 transition、定位 binary、exec
- `scripts/bench_latency.sh` 直接**删除**
- `mock/lib/` 整个子目录**删除**（全部上移到 `core/`）
- `bench.conf` 作为唯一配置源；binary 通过 `load_bench_conf()` 直读
- 用 `setns(2)` 让 binary 父进程在运行时进入 `bench_ns`（wrapper 不再 `ip netns exec`）
- 保留 `scripts/dpdk-setup.sh` / `dpdk-teardown.sh`（wrapper 内部调用，用户不直接用）

**Out of scope**：
- 重写 bench 测量核心（runner / recorder / stats 已经 OK）
- 更改 DPDK 传输实现（上一轮已实装 PMD）
- 添加新场景或新 transport
- HTTP/gRPC/TLS 支持
- 自动 build（wrapper 不触发 xmake）

---

## 技术选型

| 类别 | 选择 | 理由 |
|---|---|---|
| 语言 | C++23（同项目） | 场景 binary；无新语言 |
| Wrapper | Bash | 约 80 行；状态检测 + `ip netns` + exec，不值得 C++ |
| 配置文件 | 现有 `bench.conf`（bash KEY=VALUE） | wrapper 直接 `source`；C++ 写个 10 行解析器 |
| 网络命名空间 | Linux `setns(2)` on `/var/run/netns/bench_ns` | C++ 父进程用 setns 进 bench_ns；比 `ip netns exec` re-exec 更干净 |
| 构建 | xmake（同项目） | 每场景一个 target，`EPH_USE_DPDK` 编译双份 |
| 测试 | 手动 smoke test（每场景至少跑一次） | 集成测试本身就是端到端 bench |

---

## 架构设计

### 模块划分

```
benchmarks/latency/
├── bench.conf                       # 用户一次编辑
├── core/                            # 共享 header-only 工具
│   ├── config.hpp                   # BenchConfig + load_bench_conf()
│   ├── runner.hpp                   # BenchRunner (4-leg, Recorder)
│   ├── sample.hpp                   # RttSample / OneWaySample
│   ├── scenario_concept.hpp         # RttScenario / OneWayScenario
│   ├── signal.hpp                   # g_running + handlers
│   ├── tsc_protocol.hpp             # 24B binary header + JSON parsers
│   ├── ws_framing.hpp               # 客户端 masked build + 服务端 unmasked parse
│   ├── ws_handshake.hpp             # ← 从 mock/lib 上移
│   ├── socket_bind.hpp              # ← 合并 tcp_bind + udp_bind
│   ├── stream_scheduler.hpp         # ← 从 mock/lib 上移
│   ├── netns.hpp                    # ← 新：enter_netns("bench_ns") via setns(2)
│   └── dpdk_env.hpp                 # DPDK bootstrap (EAL / Platform / ARP)
├── tcp/
│   └── lat_tcp.cpp                  # mock + client + main，单文件约 250 行
├── udp/
│   └── lat_udp.cpp                  # 约 200 行
├── ws/
│   └── lat_ws.cpp                   # 约 300 行 (handshake 逻辑略多)
└── exchange/
    ├── mock_ws.hpp                  # 共享 mock，market + order 都 #include
    ├── mock_md_udp.hpp              # md_udp 专用
    ├── lat_ex_market.cpp            # #include mock_ws.hpp
    ├── lat_ex_order.cpp             # #include mock_ws.hpp
    └── lat_ex_md_udp.cpp            # #include mock_md_udp.hpp
```

| 模块 | 职责 | 依赖 |
|---|---|---|
| `core/` | header-only 共享工具（config、runner、TSC、CPU pin、WS 帧、socket bind、netns、DPDK env） | eph-utils, eph-dpdk (仅 dpdk_env.hpp) |
| `tcp/` `udp/` `ws/` | 三个原子场景，每个一个自足 cpp | core/ |
| `exchange/` | 三个量化场景 + 两个共享 mock header | core/ |
| `scripts/lat` | 用户唯一入口，管理 NIC-B 状态机 | `bench.conf`, `dpdk-setup.sh`, `dpdk-teardown.sh`, `ip` |
| `scripts/dpdk-setup.sh` / `dpdk-teardown.sh` | DPDK vfio-pci bind / unbind | 已有，不改 |

### 核心抽象

#### 1. `bench::BenchConfig`（扩展现有 `CommonConfig`）

```cpp
struct BenchConfig {
    // --- Networking ---
    std::string nic_a;            // NIC_A (信息性，供 log)
    std::string nic_b;            // NIC_B (同上)
    std::string server_ip;        // mock 绑定地址
    std::string local_ip;         // client 本地 IP (DPDK 需要)
    std::string gateway_ip;       // NIC-B 默认网关 (netns + DPDK)

    // --- CPU pinning ---
    int  client_cpu = 2;
    int  mock_cpu   = 4;
    std::string eal_cores = "0,1";
    bool allow_non_isolated = false;

    // --- Measurement window ---
    std::chrono::seconds warmup{2};
    std::chrono::seconds duration{10};
    long server_work_ns = 200;

    // --- Payload sweeps ---
    std::vector<size_t> tcp_payloads;
    std::vector<size_t> udp_payloads;
    std::vector<size_t> ws_payloads;
    std::vector<size_t> md_udp_payloads;
    std::vector<int>    inflights;

    // --- Exchange mock tuning ---
    std::vector<std::string> symbols = {"BTCUSDT","ETHUSDT","SOLUSDT"};
    long bookticker_us = 1000;
    long depth_ms      = 10;
    long trade_mean_ms = 5;
    long kline_s       = 1;
    size_t depth_bytes = 1024;

    // --- Scenarios / transports (仅给 wrapper 用；binary 不用) ---
    // 不放进 BenchConfig
};
```

单一 struct，每个场景 binary 读自己需要的字段，其他字段忽略。

#### 2. `bench::load_bench_conf()`

```cpp
/// Read `bench.conf` (bash-style KEY=VALUE). Lookup order:
///   1. $BENCH_CONFIG (absolute path)
///   2. ./bench.conf in cwd
///   3. walk up from /proc/self/exe to project root, check
///      benchmarks/latency/bench.conf
///
/// Returns error if no config found or required fields missing.
[[nodiscard]] std::expected<BenchConfig, std::string>
load_bench_conf();
```

必填字段（missing → error）：`nic_b`, `server_ip`, `local_ip`, `gateway_ip`, `client_cpu`, `mock_cpu`。

#### 3. `bench::enter_netns(name)`

```cpp
/// Call `setns(2)` to move the calling thread into the named network
/// namespace. `name` is resolved to `/var/run/netns/<name>`. Must be
/// called AFTER all forks (child processes inherit the original ns).
[[nodiscard]] std::expected<void, std::string>
enter_netns(std::string_view name);
```

实现：`open("/var/run/netns/bench_ns", O_RDONLY)` → `setns(fd, CLONE_NEWNET)` → `close(fd)`。需要 CAP_SYS_ADMIN，故必须以 root 运行。

#### 4. `bench::socket_bind_tcp` / `bench::socket_bind_udp`

```cpp
/// TCP: SO_REUSEADDR + TCP_NODELAY + bind + listen(backlog).
[[nodiscard]] std::expected<int, std::string>
socket_bind_tcp(std::string_view ip, uint16_t port, int backlog = 1);

/// UDP: SO_REUSEADDR + bind.
[[nodiscard]] std::expected<int, std::string>
socket_bind_udp(std::string_view ip, uint16_t port);
```

替代当前 `mock/lib/tcp_bind.hpp` + `udp_bind.hpp`，合并到 `core/socket_bind.hpp`。

### 数据流

#### 用户角度

```
vim benchmarks/latency/bench.conf        [一次]

sudo ./scripts/lat tcp
  └→ wrapper sources bench.conf
     detects NIC-B state → "kernel-host" (初态)
     transitions to "kernel-bench_ns":
         ip netns add bench_ns
         ip link set NIC-B netns bench_ns
         ip netns exec bench_ns: addr/up/route
     exec lat_tcp  (binary 在 host ns 启动)

lat_tcp 进程:
  ├─ load_bench_conf() 读同一个 bench.conf
  ├─ TSC::init
  ├─ fork()
  │   └─ child: mock::run(cfg)        # 留在 host ns，binds NIC-A (server_ip)
  ├─ parent: enter_netns("bench_ns")  # setns(2) 进入 bench_ns
  ├─ parent: client::run(cfg)         # connect(server_ip) → 经 NIC-B → VPC → NIC-A
  ├─ parent: kill(mock_pid)
  └─ parent: exit


sudo ./scripts/lat udp
  └→ detects state = "kernel-bench_ns"
     target = "kernel-bench_ns"
     无 transition
     exec lat_udp


sudo ./scripts/lat tcp --dpdk
  └→ detects state = "kernel-bench_ns"
     target = "dpdk"
     transition kernel-bench_ns → dpdk:
         destroy bench_ns (move NIC-B back)
         dpdk-setup.sh -y (bind NIC-B to vfio-pci)
     exec lat_tcp_dpdk -a <PCI> -l 0,1 -- --server-ip ...

lat_tcp_dpdk 进程:
  ├─ load_bench_conf()
  ├─ parse DPDK args (-a, -l) and app args
  ├─ EalGuard::init
  ├─ Platform::create
  ├─ ARP resolve gateway → gw_mac
  ├─ fork()
  │   └─ child: mock::run(cfg)        # 留在 host ns, POSIX socket on NIC-A
  ├─ parent: client::run(cfg, dpdk_env)   # DPDK PMD on NIC-B (vfio-pci)
  ├─ parent: kill(mock_pid)
  └─ parent: exit


sudo ./scripts/lat udp
  └→ detects state = "dpdk"
     target = "kernel-bench_ns"
     transition dpdk → kernel-bench_ns:
         dpdk-teardown.sh
         create bench_ns + move NIC-B in
     exec lat_udp
```

---

## 接口设计

### 公共 API（core/）

```cpp
// config.hpp
namespace bench {
    struct BenchConfig { /* 见上 */ };
    [[nodiscard]] std::expected<BenchConfig, std::string> load_bench_conf();
}

// netns.hpp
namespace bench {
    [[nodiscard]] std::expected<void, std::string>
    enter_netns(std::string_view name);
}

// socket_bind.hpp
namespace bench {
    [[nodiscard]] std::expected<int, std::string>
    socket_bind_tcp(std::string_view ip, uint16_t port, int backlog = 1);

    [[nodiscard]] std::expected<int, std::string>
    socket_bind_udp(std::string_view ip, uint16_t port);
}

// ws_framing.hpp (现有，不变)
namespace bench::ws_framing {
    size_t build_masked_text_frame(uint8_t* out, const void* payload,
                                   size_t len, uint32_t seed) noexcept;
    std::pair<size_t, size_t>
    parse_server_frame(const uint8_t* buf, size_t buf_len) noexcept;
}

// ws_handshake.hpp (从 mock/lib 上移，namespace 改为 bench::)
namespace bench {
    [[nodiscard]] std::expected<void, std::string>
    ws_server_handshake(int fd, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));
}

// stream_scheduler.hpp (从 mock/lib 上移，namespace 改为 bench::)
namespace bench {
    class StreamScheduler { /* 现有接口不变 */ };
}
```

### 每个 `lat_*.cpp` 的 main 模板

```cpp
#include "../core/config.hpp"
#include "../core/runner.hpp"
#include "../core/signal.hpp"
#include "../core/tsc_protocol.hpp"
#include "../core/socket_bind.hpp"
#include "../core/netns.hpp"
#include "eph/utils/cpu_pin.hpp"
#include "eph/utils/time.hpp"

#if defined(EPH_USE_DPDK)
#include "../core/dpdk_env.hpp"
#endif

using namespace bench;

// ── Mock 函数 ──
namespace mock_fn {
void run(const BenchConfig& cfg) {
    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    (void)eph::utils::pin_thread_strict(cfg.mock_cpu, "mock_tcp", policy);

    auto listen_fd = bench::socket_bind_tcp(cfg.server_ip, 19101);
    if (!listen_fd) { spdlog::error("{}", listen_fd.error()); return; }
    /* ... accept + echo loop with TSC stamping + spin_for_ns ... */
}
}

// ── Client 函数 ──
namespace client_fn {
void run(const BenchConfig& cfg) {
    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    (void)eph::utils::pin_thread_strict(cfg.client_cpu, "bench_tcp", policy);

#if defined(EPH_USE_DPDK)
    /* DPDK path: use dpdk_env to create TcpSession */
    constexpr const char* kTransport = "dpdk";
#else
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    /* connect + run TcpRttScenario via BenchRunner */
    constexpr const char* kTransport = "kernel";
#endif

    BenchRunner runner{cfg, "tcp", kTransport};
    /* runner.run_rtt_sweep(scenario, cfg.tcp_payloads); */
}
}

// ── main: fork mock + enter netns + run client ──
int main(int argc, char** argv) {
    install_signal_handlers();
    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    auto cfg_r = load_bench_conf();
    if (!cfg_r) { spdlog::error("bench.conf: {}", cfg_r.error()); return 1; }
    auto& cfg = *cfg_r;

#if defined(EPH_USE_DPDK)
    // DPDK path needs EAL args parsed from argv before --
    auto env_r = DpdkBenchEnv::create_full(argc, argv,
        cfg.server_ip, cfg.local_ip, cfg.gateway_ip, /*port_id*/ 0);
    if (!env_r) { spdlog::error("DPDK env: {}", env_r.error()); return 1; }
#endif

    // Fork the mock.
    pid_t mock_pid = fork();
    if (mock_pid < 0) { spdlog::error("fork: {}", std::strerror(errno)); return 1; }
    if (mock_pid == 0) {
        mock_fn::run(cfg);
        return 0;
    }
    // Give mock a moment to bind.
    usleep(500'000);

#if !defined(EPH_USE_DPDK)
    // Kernel transport: parent enters bench_ns.
    if (auto r = enter_netns("bench_ns"); !r) {
        spdlog::error("enter_netns: {}", r.error());
        kill(mock_pid, SIGTERM); waitpid(mock_pid, nullptr, 0);
        return 1;
    }
#endif

    client_fn::run(cfg);

    kill(mock_pid, SIGTERM);
    waitpid(mock_pid, nullptr, 0);
    return 0;
}
```

### `scripts/lat` wrapper（~100 行）

```bash
#!/usr/bin/env bash
# scripts/lat — single-command entry point for the latency bench.
#
# Usage:
#   sudo ./scripts/lat <scenario> [--dpdk] [-- <extra args to binary>]
#
# <scenario> ∈ {tcp, udp, ws, exchange/market, exchange/order, exchange/md_udp}
#
# Reads bench.conf once at the start. Detects NIC-B's current state
# (kernel-host / kernel-bench_ns / dpdk) and transitions to the mode
# the requested run needs. Transitions persist across invocations, so a
# streak of kernel runs pays the setup cost once.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="${BENCH_CONFIG:-$PROJECT_DIR/benchmarks/latency/bench.conf}"

# ... color/log helpers ...

[[ -f "$CONFIG_FILE" ]] || die "config not found: $CONFIG_FILE"
# shellcheck source=/dev/null
source "$CONFIG_FILE"

# --- Parse args ---
scenario="${1:?usage: sudo $0 <scenario> [--dpdk]}"
shift
dpdk_mode=false
[[ "${1:-}" == "--dpdk" ]] && { dpdk_mode=true; shift; }

# --- Detect NIC-B state ---
detect_state() {
    if [[ -f "$PROJECT_DIR/.dpdk_state" ]]; then
        echo "dpdk"
    elif ip netns list 2>/dev/null | grep -q '^bench_ns'; then
        echo "kernel-bench_ns"
    else
        echo "kernel-host"
    fi
}

# --- Transitions ---
setup_bench_ns() {
    ip netns add bench_ns 2>/dev/null || true
    ip link set "$NIC_B" netns bench_ns
    ip netns exec bench_ns ip addr add "${LOCAL_IP}/20" dev "$NIC_B"
    ip netns exec bench_ns ip link set "$NIC_B" up
    ip netns exec bench_ns ip link set lo up
    ip netns exec bench_ns ip route add default via "$GATEWAY_IP" dev "$NIC_B"
}
teardown_bench_ns() {
    ip netns exec bench_ns ip link set "$NIC_B" netns 1 2>/dev/null || true
    ip netns del bench_ns 2>/dev/null || true
    ip link set "$NIC_B" up 2>/dev/null || true
    ip addr add "${LOCAL_IP}/20" dev "$NIC_B" 2>/dev/null || true
}
transition() {
    local from="$1" to="$2"
    [[ "$from" == "$to" ]] && return 0
    log_info "NIC-B: $from → $to"
    case "$from" in
        dpdk)             DPDK_IFACE="$NIC_B" "$SCRIPT_DIR/dpdk-teardown.sh" ;;
        kernel-bench_ns)  teardown_bench_ns ;;
    esac
    case "$to" in
        dpdk)             DPDK_IFACE="$NIC_B" "$SCRIPT_DIR/dpdk-setup.sh" -y ;;
        kernel-bench_ns)  setup_bench_ns ;;
    esac
}

target=$([[ "$dpdk_mode" == true ]] && echo "dpdk" || echo "kernel-bench_ns")
transition "$(detect_state)" "$target"

# --- Resolve binary ---
BUILD_DIR=$(ls -d "$PROJECT_DIR"/build/linux/*/release 2>/dev/null | head -1) \
    || die "no build dir; run 'xmake build <target>' first"

# scenario name → binary key
# tcp            → lat_tcp
# exchange/order → lat_ex_order
key=$(echo "$scenario" | sed 's|^exchange/|ex_|' | tr / _)
suffix=""
[[ "$dpdk_mode" == true ]] && suffix="_dpdk"
binary="$BUILD_DIR/lat_${key}${suffix}"
[[ -x "$binary" ]] || die "binary not found: $binary"

# --- Exec ---
if [[ "$dpdk_mode" == true ]]; then
    pci=""
    [[ -f "$PROJECT_DIR/.dpdk_state" ]] && { source "$PROJECT_DIR/.dpdk_state"; pci="$DPDK_PCI"; }
    exec "$binary" -a "$pci" -l "${EAL_CORES:-0,1}" -- "$@"
else
    exec "$binary" "$@"
fi
```

**Binary 运行在 host 命名空间**，parent 进程在 fork mock 之后自己调 `setns(2)` 进入 `bench_ns`。wrapper 不使用 `ip netns exec`——简洁且 fork 语义清楚。

### 错误体系

统一 `std::expected<T, std::string>`（已是项目惯例）。错误必须说明**事实 + 可能原因 + 下一步**：

```cpp
return std::unexpected(
    "bench.conf: missing required field NIC_B. "
    "Copy and edit benchmarks/latency/bench.conf.example to bench.conf.");
```

---

## 编码规范

| 维度 | 规范 |
|---|---|
| 命名空间 | `bench::` 顶层；`bench::mock_fn` / `bench::client_fn` 仅在场景 cpp 内匿名内联使用（不导出到 header） |
| 文件命名 | `lat_<scenario>.cpp`：`tcp/lat_tcp.cpp`, `udp/lat_udp.cpp`, `ws/lat_ws.cpp`, `exchange/lat_ex_{market,order,md_udp}.cpp` |
| Binary 命名 | `lat_<scenario>` (kernel) / `lat_<scenario>_dpdk` (DPDK)，scenario 用下划线形式（`ex_market` 对应 `exchange/market`） |
| 类型 | `PascalCase` |
| 函数 / 变量 | `snake_case` |
| 常量 | `kCamelCase` |
| 错误处理 | 启动期 `std::expected`；hot-path bool |
| 日志 | spdlog；启动 INFO；结果 INFO；恢复路径 WARN；致命 ERROR |
| 注释 | 只注释 *why*，不注释 *what* |

---

## 实施计划

> **Commit 策略**：每阶段完成后 `/git` 提交，commit message 标注阶段编号。每阶段是独立回滚点。

### 阶段 1：Finalize 未提交的小清理

**当前 working tree** 已有若干未提交的改动（删 FrameReader / 合并 ws_framing / 内联 serve_busy_poll / 删 loopback 模式）。先 build + verify + commit 这些改动，作为重构起点。

- 交付物：干净的 working tree，commits 已落地
- 验收标准：`git status` 干净，`xmake build` 通过，kernel smoke test 跑得动
- 推荐 skill：手动 commit（我已有改动）
- 注意：此阶段产物会在后续阶段进一步被重构，但 commit 是独立回滚点

### 阶段 2：core/ 扩充 — 移入小工具

创建 / 搬迁：
- `core/socket_bind.hpp` — 合并 `mock/lib/tcp_bind.hpp` + `mock/lib/udp_bind.hpp`，namespace 从 `bench::mock::` 改为 `bench::`
- `core/ws_handshake.hpp` — 从 `mock/lib/ws_handshake.hpp` 上移，namespace 改 `bench::`
- `core/stream_scheduler.hpp` — 从 `mock/lib/stream_scheduler.hpp` 上移，namespace 改 `bench::`
- `core/netns.hpp` — **新**：`bench::enter_netns(name)` via `setns(2)`

删除：
- `benchmarks/latency/mock/` 整个目录（只剩 ws_frame.hpp 的内容，已合并到 core/ws_framing.hpp）

更新所有 #include。

- 交付物：上述文件 + 所有 mock.cpp 的 include 更新
- 验收标准：现有 `mock_lat_*` / `bench_lat_*` 全套 build 通过
- 推荐 skill：手动 refactor（批量 sed + build）

### 阶段 3：`core/config.hpp` 扩展 — `load_bench_conf()`

在 `core/config.hpp` 里：
- 用 `BenchConfig`（大 struct，含 payload sweeps、exchange 字段）替代原 `CommonConfig`
  - 或保留 `CommonConfig` 作为 alias
- 实现 `load_bench_conf()`：
  - 按 `BENCH_CONFIG` → `./bench.conf` → `$PROJECT_ROOT/benchmarks/latency/bench.conf` 顺序找文件
  - 简单 KEY=VALUE 解析（忽略 `#` 注释，strip 空白）
  - CSV 字段 (`TCP_PAYLOADS` 等) 解析为 vector
  - 必填校验（NIC_B / SERVER_IP / LOCAL_IP / GATEWAY_IP / CLIENT_CPU / MOCK_CPU）

写 2-3 个单测到 `tests/unit/bench/test_load_bench_conf.cpp`（valid / missing required / CSV parsing）。

- 交付物：`config.hpp` 扩展 + 单测
- 验收标准：单测通过；现有 binary 仍能 build（parse_common 仍存在，新接口并行）
- 推荐 skill：`/design` 或手动

### 阶段 4：合并场景 — tcp / udp / ws

对每个场景目录：
- 创建 `lat_<scenario>.cpp`：`#include` 必要 header，定义 `mock_fn::run` + `client_fn::run`（两个 namespace 保持清晰边界），main 函数做 TSC init + load config + fork + setns + run client + cleanup
- 场景特定的逻辑（scenario class 当前在 `scenario.hpp` 和 `dpdk_scenario.hpp`）合并到同一个 `.cpp` 里，用 `#ifdef EPH_USE_DPDK` 切换
- 删除该目录下的 `mock.cpp`, `bench.cpp`, `scenario.hpp`, `dpdk_scenario.hpp`, `client.hpp`

结果：每个场景目录只有 1 个 cpp 文件。

- 交付物：3 个 `lat_*.cpp`，3 个场景目录只剩单文件
- 验收标准：3 × 2 = 6 个 target build 通过（`lat_tcp` / `lat_tcp_dpdk` / ...）；手动 smoke test kernel 模式
- 推荐 skill：`/design`

### 阶段 5：合并场景 — exchange

exchange 稍复杂因为 market 和 order 共享 mock：

- `exchange/mock_ws.hpp` — 从 `exchange/mock_ws.cpp` 重构：暴露 `void run_exchange_ws_mock(const BenchConfig&)` 函数（签名明确，接受 config，无 main）
- `exchange/mock_md_udp.hpp` — 同上
- `exchange/lat_ex_market.cpp` — `#include "mock_ws.hpp"`，main 里 fork + 子进程调 `run_exchange_ws_mock(cfg)`，父进程跑 market RX scenario
- `exchange/lat_ex_order.cpp` — 同上但父进程跑 order RTT scenario
- `exchange/lat_ex_md_udp.cpp` — `#include "mock_md_udp.hpp"`，父进程跑 md_udp scenario
- 删除 `exchange/mock_ws.cpp`, `mock_md_udp.cpp`, `bench_market.cpp`, `bench_order.cpp`, `bench_md_udp.cpp`, `scenario.hpp`, `dpdk_scenario.hpp`

结果：exchange 目录 = 2 个 hpp + 3 个 cpp。

- 交付物：5 个文件替代原 9 个
- 验收标准：3 × 2 = 6 个 exchange target build 通过；smoke test exchange/market + exchange/order
- 推荐 skill：`/design`

### 阶段 6：xmake.lua 重写

- 删除所有旧 target（`mock_lat_*`, `bench_lat_*`）
- 添加 12 个新 target：
  ```
  lat_tcp / lat_tcp_dpdk
  lat_udp / lat_udp_dpdk
  lat_ws / lat_ws_dpdk
  lat_ex_market / lat_ex_market_dpdk
  lat_ex_order / lat_ex_order_dpdk
  lat_ex_md_udp / lat_ex_md_udp_dpdk
  ```
- 每个 target 一个 `lat_*.cpp`；kernel/DPDK 用 `add_defines("EPH_USE_DPDK=1")` 分离
- 保留 tests/unit/bench/xmake.lua 的现有单测

- 交付物：新 xmake.lua section
- 验收标准：`xmake build` 通过，12 个 target 全出二进制
- 推荐 skill：手动

### 阶段 7：`scripts/lat` wrapper

写 `scripts/lat`（约 100 行）：
- Source bench.conf
- Parse `<scenario>` + `--dpdk`
- Detect NIC-B state
- Transition if needed（调 dpdk-setup/teardown 或 `ip netns`）
- Resolve binary path
- Exec

初次自动创建 bench_ns 时打印：
```
ℹ  NIC-B: kernel-host → kernel-bench_ns
```

- 交付物：`scripts/lat` + chmod +x
- 验收标准：
  - `sudo ./scripts/lat tcp` — kernel 首次自动创建 bench_ns，binary 运行，输出 4-leg 报告
  - `sudo ./scripts/lat udp` — 复用 bench_ns（无 transition 日志）
  - `sudo ./scripts/lat tcp --dpdk` — 自动 dpdk-setup，binary 运行
  - `sudo ./scripts/lat udp` — 自动 dpdk-teardown + 创建 bench_ns
- 推荐 skill：`/script`

### 阶段 8：删除 `scripts/bench_latency.sh`

- 删除 `scripts/bench_latency.sh`
- 删除 `benchmarks/latency/bench.conf` 里 `SCENARIOS=` / `TRANSPORTS=` 字段（wrapper 不用它们）
- 更新 README/docs 里所有 `bench_latency.sh` 提及

- 交付物：一次 commit
- 验收标准：grep `bench_latency` 在源码内无结果
- 推荐 skill：手动

### 阶段 9：端到端验证

真实网线背对背（ens34/ens35）全套跑一次：
- `sudo ./scripts/lat tcp`
- `sudo ./scripts/lat udp`
- `sudo ./scripts/lat ws`
- `sudo ./scripts/lat exchange/market`
- `sudo ./scripts/lat exchange/order`
- `sudo ./scripts/lat exchange/md_udp`
- `sudo ./scripts/lat tcp --dpdk`（触发 kernel → dpdk 切换）
- `sudo ./scripts/lat udp --dpdk`
- `sudo ./scripts/lat exchange/market --dpdk`
- `sudo ./scripts/lat tcp`（触发 dpdk → kernel 切换）

每次 transition 应打印清楚，数字应与 commit `37a10ff` 的基线一致（±10% 内，考虑 AWS 抖动）。

- 交付物：一份 bench 数据对比（旧 vs 新）存入 `.artifacts/`
- 验收标准：所有场景 RTT p50 与基线偏差 < 10%
- 推荐 skill：手动

---

## 关键决策记录

### D-1: 每场景一个 cpp，内部 namespace 隔离

- **问题**：mock 和 client 代码是否应该放在同一文件
- **选项**：
  - A. 分离：`mock.hpp` + `client.hpp` + `main.cpp` 三文件
  - B. 合并：单个 `lat_<scene>.cpp` + `mock_fn::` / `client_fn::` namespace
  - C. mock 作为独立 binary，client binary fork+exec
- **决策**：B
- **理由**：mock 和 client 永远一起被阅读/修改；拆 3 文件只会增加跳转；namespace 提供足够的语义隔离；R14 和 R1 在 /discuss 里达成一致
- **验收标准**：每个场景目录（tcp/udp/ws）只有 1 个 cpp 文件

### D-2: 用 setns(2) 而非 `ip netns exec`

- **问题**：如何让 bench client 进 bench_ns
- **选项**：
  - A. wrapper 调 `ip netns exec bench_ns ./binary`（类似当前）
  - B. binary 在 fork 完 mock 后自己调 `setns(2)`
- **决策**：B
- **理由**：
  1. Binary 在 host ns 启动 → fork mock（子进程留 host，binds NIC-A）→ 父进程 setns 进 bench_ns（可见 NIC-B）。语义直接，不需要额外 wrapper。
  2. 避免 wrapper 和 binary 之间的 env/arg 传递问题。
  3. `setns` 只是 1 个 syscall，比 exec 一个额外 ip 进程便宜得多。
- **验收标准**：`enter_netns("bench_ns")` 后 `ip link show` 在 bench_ns context，可见 NIC-B

### D-3: Wrapper 持久化状态，不在退出时回滚

- **问题**：每次 bench 结束后要不要自动恢复 NIC-B 到 host 默认状态
- **选项**：
  - A. 每次 binary 退出自动 teardown（每次付 ~600ms 代价）
  - B. wrapper 只在"mode 切换"时 transition，持久化当前状态（streak of same-mode runs 零开销）
- **决策**：B
- **理由**：典型使用是"连续跑多个 kernel 场景"或"连续跑多个 DPDK 场景"，持久化让 streak 运行无开销；只在模式切换时付出 transition 成本，用户无感
- **验收标准**：连续 5 次 `sudo ./scripts/lat tcp` 只在第一次打印 "transitioning"，后续 4 次直接跑

### D-4: 2 binary 分离（kernel / dpdk），不合并

- **问题**：能否单一 binary 通过 runtime flag 切 kernel/DPDK
- **选项**：
  - A. 单 binary + `--dpdk` flag
  - B. 2 binary：`lat_<s>` 与 `lat_<s>_dpdk`，编译期 `EPH_USE_DPDK` 分离
- **决策**：B
- **理由**：eph-dpdk 静态 link 约 80 MB（含 PMD）；合并后 kernel binary 启动也要 EAL init 扫 PCI；R1 在 /discuss 里用数据说服了极简派
- **验收标准**：`ls -lh build/.../lat_tcp` 约 5 MB，`lat_tcp_dpdk` 约 80 MB，两者独立 target

### D-5: 保留 `bench.conf` 作为唯一配置源，删除 `--scenarios` / `--transports` 等 CLI flag

- **问题**：wrapper 是否保留 CLI flag 做运行时过滤
- **选项**：
  - A. wrapper 只接受 `<scenario>` + `--dpdk`，所有 tuning 在 bench.conf
  - B. wrapper 保留 `--duration` / `--payloads` / `--bookticker-us` 等 flag
- **决策**：A
- **理由**：用户的核心诉求是"简单直接"；CLI flag 越多越偏离这个目标；用户偶尔需要 tuning 时改 bench.conf 几秒的事
- **验收标准**：`sudo ./scripts/lat --help` 只展示 2 个 flag：`<scenario>` + `--dpdk`

### D-6: 删除 `bench-setup.sh` / `bench-teardown.sh`

- **问题**：是否对称于 dpdk-setup/teardown 提供 kernel 模式的手动脚本
- **选项**：
  - A. 提供对称脚本（用户也可以 / 也不可以直接用）
  - B. 不提供；所有 netns 生命周期由 wrapper 管理
- **决策**：B
- **理由**：R1 担心"逃生门"但自愈逻辑已经能处理残留——下次 wrapper 调用检测到 stale state 会修复；另外用户反馈指出"多脚本就是复杂度"
- **验收标准**：`scripts/` 下只有 `lat` / `dpdk-setup.sh` / `dpdk-teardown.sh` 三个 bench 相关脚本

### D-7: Mock 始终在 host namespace 跑

- **问题**：fork 之后 mock 子进程应该在哪个 ns
- **选项**：
  - A. Mock 留 host ns（binds NIC-A 的 server_ip），client 进 bench_ns
  - B. Mock 进 bench_ns
- **决策**：A
- **理由**：
  1. NIC-A 在 host ns，mock 必须能 bind 它
  2. Client 跑 bench_ns（绑 NIC-B）连接 server_ip 时，路由表把流量通过 NIC-B 出去，经 VPC fabric 返回 NIC-A
  3. 这才是"真实网线背对背"的正确语义
- **验收标准**：`ss -tlnp` 在 host ns 显示 mock 监听 server_ip；在 bench_ns 看不到

---

## 不确定点（实施时可能发现需要调整）

- **DPDK binary 的 mock 子进程**：mock 用 POSIX bind NIC-A，但 fork 发生在 EAL init 之后 —— 可能与 DPDK 的 hugepages 映射有冲突（DPDK 会 lock 大量内存）。
  - **应急方案**：如果 fork 在 EAL init 后失败，改成 EAL init *之前* fork，或者用 `vfork` + `execl("./mock_lat_tcp", ...)` 跑独立 mock binary 保持现状。
  - **判断时机**：阶段 4 smoke test 时验证 DPDK 下的 fork 可行性

- **`enter_netns(2)` 的 CAP_SYS_ADMIN 要求**：已 root 运行，应该没问题

- **bench.conf 必填字段校验**：`load_bench_conf` 的错误消息要非常具体（"missing NIC_B" 而非 "missing required field"），否则用户会困惑

---

## 阶段数与回滚

| 阶段 | 预估 | 阻塞后续 |
|---|---|---|
| 1. Finalize 未提交 cleanup | 15 min | 所有 |
| 2. core/ 扩充 + 删 mock/lib | 30 min | 3,4,5 |
| 3. load_bench_conf() | 45 min | 4,5 |
| 4. tcp/udp/ws 合并 | 1-2 h | 6,9 |
| 5. exchange 合并 | 1-2 h | 6,9 |
| 6. xmake.lua 重写 | 20 min | 7,9 |
| 7. scripts/lat wrapper | 30 min | 8,9 |
| 8. 删除 bench_latency.sh | 10 min | — |
| 9. 端到端验证 | 30 min | — |

总计约 5-7 小时。每阶段独立 commit，任意阶段失败可回退。
