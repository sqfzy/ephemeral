# Plan: benchmarks/latency/ 完全重构

> 删除现有 latency bench 全部代码，按 6 场景 × 2 transport × 多 payload 的矩阵从零重写，明确"普通业务"与"加密交易所"两类 mock 的语义、抽象、目录、绑核、与数据流约定。

创建时间：2026-04-09
状态：已确认

---

## 定位与边界

**目标**：为 `eph-net` / `eph-dpdk` 提供一套结构清晰、可信度高、长期可维护的延迟基准，覆盖 6 个真实场景的 kernel vs DPDK 对比。

**用户**：内部开发者，用于性能回归与优化决策。

**In scope**：
- 6 个场景的 client bench：`tcp` / `udp` / `ws` / `exchange/market` / `exchange/order` / `exchange/md_udp`
- 5 个 mock server 二进制：`mock_tcp` / `mock_udp` / `mock_ws` / `mock_exchange_ws` / `mock_exchange_md_udp`
- 强校验的 CPU 绑核（isolcpus + sibling + NUMA）
- 4-leg 延迟报告（TX / RX / RTT / Server-leg）通过 TSC 时间戳测量
- HdrHistogram 百分位（p50 / p99 / p999 / max）
- 编排脚本 `scripts/bench_latency.sh`（preflight + netns/dpdk 切换 + 启停 mock + 跑 client）

**Out of scope**：
- 文件落盘（`.bench/`、`HISTORY.md`、JSONL）— 用户自行 `tee`/`>` stdout
- 真实交易所 trace 回放（`mock_exchange_ws --trace FILE` 留接口位但初版不实现）
- C 级业务工作负载模拟（哈希、查表）— 仅 spin N 个 cycle
- 双 DPDK 网卡同时收发（无硬件支持，UDP 行情转发用 echo 形式）
- 自动化的延迟回归对比（阶段 6 手动跑一次，长期靠用户）

---

## 技术选型

| 类别 | 选择 | 理由 |
|---|---|---|
| 语言 | C++23 | 项目约定 |
| 依赖 | `eph-utils`（TSC、HdrHistogram、CPU pin）、`eph-net`（client transport）、`eph-dpdk`（DPDK build 专用）、`spdlog` | 复用现有基础设施 |
| 构建 | xmake，每场景双 target（kernel + `_dpdk`）通过 `add_defines("EPH_USE_DPDK=1")` 切换 | 编译期分支让 DPDK 优化能内联 |
| 测试 | xmake test + 集成 smoke test（手动跑 mock + bench 验证 4-leg 报告非零） | bench 本身就是端到端测试 |
| 编排 | bash 脚本 `scripts/bench_latency.sh` | 现有脚本基础设施 |

---

## 架构设计

### 模块划分

```
benchmarks/latency/
├── core/                   # 共享基础（headers only）
│   ├── config.hpp          # CommonConfig + DpdkConfig + 场景特定 config
│   ├── sample.hpp          # RttSample / OneWaySample
│   ├── runner.hpp          # BenchRunner + 三个 sweep 入口
│   ├── scenario_concept.hpp # RttScenario / OneWayScenario concept
│   ├── hist_report.hpp     # HdrHistogram 包装 + spdlog 报告
│   ├── cpu_pin.hpp         # pin_thread_strict + CpuPinPolicy
│   ├── signal.hpp          # g_running + SIGINT/SIGTERM
│   ├── tsc_protocol.hpp    # 二进制 + JSON 两种 TSC 协议帧
│   └── timer.hpp           # warmup/measurement 计时器
├── mock/lib/               # mock 共享层（裸 socket，不依赖 eph-net）
│   ├── busy_poll.hpp       # accept + busy-poll 主循环骨架
│   ├── tcp_bind.hpp        # SO_REUSEADDR + TCP_NODELAY 引导
│   ├── udp_bind.hpp        # UDP socket 引导
│   ├── ws_handshake.hpp    # 服务端 WebSocket 握手
│   ├── ws_frame.hpp        # 服务端 WS 帧 build/parse
│   ├── work_spin.hpp       # spin N ns（pause 指令循环）
│   └── stream_scheduler.hpp # delta-timer 优先队列（量化 mock 用）
├── tcp/                    # 普通 TCP 场景
│   ├── mock.cpp            # mock_tcp / mock_tcp_dpdk
│   ├── bench.cpp           # bench_tcp / bench_tcp_dpdk
│   └── scenario.hpp        # TcpRttScenario
├── udp/                    # 普通 UDP 场景（同上结构）
├── ws/                     # 普通 WebSocket 场景（同上结构）
└── exchange/               # 量化业务（3 场景共享一个目录）
    ├── mock_ws.cpp         # mock_exchange_ws / mock_exchange_ws_dpdk
    ├── mock_md_udp.cpp     # mock_exchange_md_udp / mock_exchange_md_udp_dpdk
    ├── bench_market.cpp    # bench_exchange_market（OneWay）
    ├── bench_order.cpp     # bench_exchange_order（RTT inflight sweep）
    ├── bench_md_udp.cpp    # bench_exchange_md_udp（RTT payload sweep）
    └── scenario.hpp        # MarketRxScenario / OrderRttScenario / MdUdpScenario
```

| 模块 | 职责 | 依赖 |
|---|---|---|
| `core` | 复用工具，所有 bench/mock cpp 都 include | `eph-utils`（TSC, HdrHistogram, CPU pin） |
| `mock/lib` | mock 服务端裸 socket 工具 | 无（仅 POSIX + spdlog） |
| `<scenario>/scenario.hpp` | 实现 `RttScenario` 或 `OneWayScenario` concept | `core/`, `eph-net/eph-dpdk` |
| `<scenario>/mock.cpp` | mock main：解析 → 绑核 → busy-poll 主循环 | `core/`, `mock/lib/` |
| `<scenario>/bench.cpp` | client main：解析 → 绑核 → 创建 transport → 跑 runner | `core/`, `eph-net/eph-dpdk` |

**依赖方向严格单向**：`bench.cpp` / `mock.cpp` → `core/` + `mock/lib/` → 项目库 → 系统库。`mock/lib/` **不依赖** `core/` 中的任何 client-side 工具，**不依赖** `eph-net`（mock 必须确定性裸 socket）。

### 核心抽象

#### 1. 两个 Scenario concept

```cpp
namespace bench {

template<typename T>
concept RttScenario = requires(T& s, size_t payload, RttSample& out) {
    { s.prepare(payload) } -> std::same_as<bool>;
    { s.do_one_rtt(out)  } -> std::same_as<bool>;
    { s.cleanup()        } -> std::same_as<void>;
};

template<typename T>
concept OneWayScenario = requires(T& s, OneWaySample& out) {
    { s.prepare()        } -> std::same_as<bool>;
    { s.do_one_recv(out) } -> std::same_as<bool>;
    { s.cleanup()        } -> std::same_as<void>;
};

} // namespace bench
```

**设计意图**：
- 拒绝当前"用 payload=0 假装 RTT"的 hack
- 编译期 dispatch（template + concept），hot path 完全内联，零虚函数

#### 2. 测量样本

```cpp
struct RttSample {
    uint64_t client_send_tsc{};
    uint64_t server_recv_tsc{};   // 0 = 该 leg 不可用
    uint64_t server_send_tsc{};   // 0 = 该 leg 不可用
    uint64_t client_recv_tsc{};
};

struct OneWaySample {
    uint64_t producer_tsc{};      // server 发送时 stamp
    uint64_t consumer_tsc{};      // client 接收时 stamp
};
```

#### 3. Runner（三个 sweep 入口）

```cpp
class BenchRunner {
public:
    BenchRunner(CommonConfig cfg,
                std::string_view scenario_name,
                std::string_view transport_name);

    // 用于 tcp / udp / ws / exchange/md_udp
    template<RttScenario S>
    void run_rtt_sweep(S& s, std::span<const size_t> payloads);

    // 用于 exchange/order：固定 payload，sweep inflight 维度
    template<RttScenario S>
    void run_rtt_inflight_sweep(S& s, std::span<const int> inflights);

    // 用于 exchange/market：单次 1-leg 测量
    template<OneWayScenario S>
    void run_oneway(S& s);
};
```

**通用循环骨架**（三个入口共用）：
1. `prepare(...)`
2. **pre-warmup**：丢弃前 N 轮（默认 2000 轮）冷启动
3. **timer warmup**：`cfg.warmup` 秒内收样不计入
4. **measurement**：`cfg.duration` 秒内样本进 HdrHistogram
5. `compute_stats()` → `print_bench_result()`（spdlog INFO）
6. histogram reset → `cleanup()`
7. 下一个 sweep 点

#### 4. CPU 绑核

```cpp
struct CpuPinPolicy {
    bool require_isolcpus           = true;
    bool require_no_sibling_conflict = true;
    bool require_same_numa          = true;
    bool warn_irq_overlap           = true;
};

[[nodiscard]] std::expected<void, std::string>
pin_thread_strict(int cpu, std::string_view name, CpuPinPolicy policy);
```

**校验逻辑**：
1. 读 `/sys/devices/system/cpu/isolated` → 若 cpu ∉ list，违反 `require_isolcpus` 时返回 error
2. 读 `/sys/devices/system/cpu/cpuN/topology/thread_siblings_list` → 若任一 sibling 已被本进程其他绑核线程占用，违反 `require_no_sibling_conflict` 时返回 error
3. 读 `/sys/devices/system/cpu/cpuN/node*` → 若与已绑核线程不在同一 NUMA node，违反 `require_same_numa` 时返回 error
4. 读 `/proc/interrupts` → 若 cpu 列有 IRQ 计数 > 0，`warn_irq_overlap=true` 时 spdlog::warn
5. `sched_setaffinity` 后立刻 `sched_getaffinity` 双向校验
6. `pthread_setname_np` 设线程名

CLI `--allow-non-isolated` 把 `CpuPinPolicy` 的 `require_isolcpus` 关掉（仅供开发机临时用）。

#### 5. CLI 配置（按场景类别拆开，不再大杂烩）

```cpp
// 所有 bench/mock 共享
struct CommonConfig {
    std::string server_ip;
    uint16_t    server_port = 0;
    std::chrono::seconds warmup{2};
    std::chrono::seconds duration{10};
    int  client_cpu = -1;          // bench client 用
    int  mock_cpu   = -1;          // mock 进程用
    long server_work_ns = 0;       // mock 进程用（仅 RTT mock 生效）
    bool allow_non_isolated = false;
};

// 仅 DPDK build 使用（编译期 #ifdef EPH_USE_DPDK 包含）
struct DpdkConfig {
    std::string local_ip;
    std::string gateway_ip;
    std::string eal_cores = "0,1";
    uint16_t    dpdk_port_id = 0;
};

// 场景特定：仅在对应场景的 main 中解析
struct WsExchangeConfig {
    std::vector<std::string> symbols      = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    std::chrono::microseconds bookticker_us{100};
    std::chrono::milliseconds depth_ms{10};
    std::chrono::milliseconds trade_mean_ms{5};
    std::chrono::seconds       kline_s{1};
    size_t depth_payload_bytes = 1024;
};

struct OrderConfig {
    std::vector<int> inflights = {1, 4, 16, 64};
};
```

CLI 优先级：**命令行 > 环境变量 > 默认值**。环境变量名 `BENCH_<UPPER>`（如 `BENCH_CLIENT_CPU` 对应 `--client-cpu`）。

### 数据流

```
mock 进程：
  parse_cli → tsc_init → pin_strict(mock_cpu)
  → tcp/udp/ws_bind → busy_poll_loop {
       recv() → optional<work_spin(ns)> → send() → schedule_next_push()
    }

bench client 进程：
  parse_cli → tsc_init → pin_strict(client_cpu)
  → eph::net/dpdk transport.connect()
  → BenchRunner::run_*_sweep(scenario, ...)
       → for each sweep point:
            scenario.prepare() → pre_warmup → timer_warmup → measure
            → HdrHistogram → spdlog INFO 报告
  → transport.stop()
```

测量样本不出进程，全部 in-memory；最终 spdlog INFO 输出到 stdout。**不写任何文件**。

---

## 接口设计

### 公共 API（5 个核心入口）

```cpp
// === core/config.hpp ===
[[nodiscard]] CommonConfig parse_common(int argc, char** argv);
#ifdef EPH_USE_DPDK
[[nodiscard]] DpdkConfig   parse_dpdk(int argc, char** argv);
#endif

// === core/cpu_pin.hpp ===
[[nodiscard]] std::expected<void, std::string>
pin_thread_strict(int cpu, std::string_view name, CpuPinPolicy policy = {});

// === core/runner.hpp ===
class BenchRunner {
public:
    BenchRunner(CommonConfig cfg,
                std::string_view scenario_name,
                std::string_view transport_name);
    template<RttScenario S>
        void run_rtt_sweep(S&, std::span<const size_t> payloads);
    template<RttScenario S>
        void run_rtt_inflight_sweep(S&, std::span<const int> inflights);
    template<OneWayScenario S>
        void run_oneway(S&);
};

// === core/tsc_protocol.hpp ===
namespace bench::tsc {
    inline constexpr size_t kBinaryHeaderSize = 24;
    void     stamp_binary(uint8_t* buf, uint64_t client_send,
                          uint64_t server_recv, uint64_t server_send);
    void     parse_binary(const uint8_t* buf,
                          uint64_t& client_send,
                          uint64_t& server_recv,
                          uint64_t& server_send);
    uint64_t parse_T(const uint8_t* json, size_t len);
    uint64_t parse_T_recv(const uint8_t* json, size_t len);
    uint64_t parse_T_send(const uint8_t* json, size_t len);
}

// === mock/lib/work_spin.hpp ===
inline void work_spin(long ns) noexcept;  // pause-loop until N ns elapsed
```

### 错误体系

- **`std::expected<T, std::string>`** 用于可恢复错误（绑核失败、socket 绑定失败、握手失败）
- **`std::abort()` / `std::exit(1)`** 用于不可恢复的启动错误（TSC 校准失败、CLI 必填缺失）
- **bool 返回值** 用于 hot-path（`do_one_rtt` / `do_one_recv`），失败 = 跳过本轮、继续循环；连续失败由 runner 累计 + spdlog::warn
- **不抛异常**：所有 hot-path 与 runner 路径 noexcept

### Mock CLI 共享参数

所有 5 个 mock 二进制都接受：
```
--bind-ip IP          (必填)
--port N              (必填)
--mock-cpu N          (必填)
--server-work-ns N    (默认 200)
--allow-non-isolated  (开发用)
```

`mock_exchange_ws` 额外接受：
```
--symbols SYM,...
--bookticker-us US    (默认 100)
--depth-ms MS         (默认 10)
--trade-mean-ms MS    (默认 5)
--kline-s S           (默认 1)
--depth-bytes N       (默认 1024)
--trace FILE          (留接口位，初版返回 "not implemented")
```

### Bench client CLI 共享参数

所有 6 个 bench 二进制都接受：
```
--server-ip IP        (必填)
--port N              (必填)
--warmup SEC          (默认 2)
--duration SEC        (默认 10)
--client-cpu N        (必填)
--allow-non-isolated  (开发用)
```

各场景额外参数：
- `bench_tcp/udp/ws/md_udp`：`--payload-sizes 64,128,...`
- `bench_exchange_market`：`--symbols`（client 端记录用，不发送）
- `bench_exchange_order`：`--inflights 1,4,16,64`
- DPDK build 额外：`--local-ip IP --gateway-ip IP --eal-cores L,L --dpdk-port N`

---

## 编码规范

| 维度 | 规范 |
|---|---|
| 命名空间 | `bench::` 顶层；mock 内部用 `bench::mock::`（仅 mock cpp + mock/lib 用）；scenario 类型用 `bench::scenario::` |
| 二进制命名 | mock：`mock_<proto>` 或 `mock_exchange_<role>`；client bench：`bench_<scenario>`；DPDK 变体后缀 `_dpdk` |
| 文件命名 | 一个场景目录内固定 3 文件名：`mock.cpp` / `bench.cpp` / `scenario.hpp`（exchange 目录下 mock/bench 加 `_<role>` 后缀） |
| 类型 | `PascalCase`；concept 命名 `XxxScenario`（不带 `-Like` 后缀） |
| 函数 | `snake_case` |
| 常量 | `kCamelCase` |
| CLI 参数 | `--kebab-case` |
| 环境变量 | `BENCH_<UPPER_SNAKE>` |
| 错误处理 | 启动期 `std::expected`；hot-path bool；不抛异常 |
| 日志 | spdlog；启动 INFO；结果 INFO；恢复路径 WARN；致命 ERROR；hot-path 内 SPDLOG_DEBUG（编译关闭） |
| 注释 | 头部 `/// @file` + 设计意图；非平凡逻辑解释 *why*；不注释 *what* |
| Header-only | `core/` 与 `mock/lib/` 全部 header-only；只在 `*.cpp` 中实例化 |
| Hot-path | runner 测量循环禁止 `std::function` / 虚函数 / 堆分配；样本通过引用传递 |

---

## Payload Size 矩阵

| 场景 | sweep 维度 | 默认值 | 理由 |
|---|---|---|---|
| `tcp` | payload bytes | `64,128,256,512,1024,1460,4096,16384` | 1460 = MTU 内单包；4K/16K 触发 TCP 多包/分段 |
| `udp` | payload bytes | `64,128,256,512,1024,1472` | 1472 = MTU 上限（IP+UDP header 28），UDP 不分片 |
| `ws` | payload bytes | `64,128,256,512,1024,4096` | 覆盖 WS 帧头两种长度编码（≤125 单字节、126–65535 两字节） |
| `exchange/market` | 无 | N/A | OneWay；payload 由 mock stream 类型决定（bookTicker 150 / depth 1024 / trade 200 / kline 300） |
| `exchange/order` | inflight | `1,4,16,64` | order 帧固定 ~200B；sweep 并发度看 p99 在压力下的恶化；`--inflights 1` 即同步基线 |
| `exchange/md_udp` | payload bytes | `64,256,1024,1400` | 行情 payload 三个层次：bookTicker / depth-update / depth-snapshot |

---

## CPU 绑核策略

### 默认核分配

```
client_cpu = 2     (bench client poll thread)
mock_cpu   = 4     (mock server busy-poll thread)
eal_cores  = 0,1   (DPDK EAL main + worker)
```

### 强校验项（默认全开）

| 校验 | 失败处理 | 来源 |
|---|---|---|
| cpu 在 `/sys/devices/system/cpu/isolated` 列表 | error，退出 | 防 OS scheduler 抢核 |
| cpu 与本进程其他绑核线程不共享 HT sibling | error，退出 | 防 SMT 共享物理核 |
| cpu 与本进程其他绑核线程同 NUMA node | error，退出 | 防跨 NUMA 内存访问 |
| cpu 在 `/proc/interrupts` 上 IRQ 计数 = 0 | warn | NIC IRQ 抖动源 |
| `sched_setaffinity` 后 `sched_getaffinity` 一致 | error，退出 | 设置失败防御 |

### 软关开关

`--allow-non-isolated` 把 `require_isolcpus` 关掉（其他校验保留）。仅开发机临时用。

### 配置来源优先级

CLI > env > 默认值。env 名：`BENCH_CLIENT_CPU` / `BENCH_MOCK_CPU` / `BENCH_EAL_CORES`。

---

## Mock Server 架构

### 共有特征

- **裸 POSIX socket**（不依赖 `eph-net`）；DPDK build 直接调 `rte_eth_*`
- **单线程 busy-poll**：`while (running) { recv(MSG_DONTWAIT); if (RTT mock) work_spin(ns); send(); maybe_schedule(); }`
- **绑核 + 强校验**（`mock/lib/busy_poll.hpp` 入口先 `pin_thread_strict`）
- **TCP_NODELAY + SO_REUSEADDR**（TCP/WS）
- **TSC 时间戳**（统一通过 `core/tsc_protocol.hpp` 读写）

### 5 个 mock 二进制

| 二进制 | workload | 说明 |
|---|---|---|
| `mock_tcp` | 收 N 字节 → spin work_ns → 回 N 字节 | N 来自客户端首字节协商或固定头 |
| `mock_udp` | 收 datagram → spin work_ns → 回 datagram | 同上 |
| `mock_ws` | 解 WS 帧 → spin work_ns → 编 WS 帧回 | 服务端不 mask（RFC 6455） |
| `mock_exchange_ws` | 多 stream 调度行情 push + 收到 order 时回 ExecutionReport（spin work_ns） | 用 `stream_scheduler` delta-timer 优先队列 |
| `mock_exchange_md_udp` | UDP echo with 行情 payload | 与 `mock_udp` 同结构，payload 是行情数据格式 |

### 加密交易所 mock 流量模型

`mock_exchange_ws` 实现以下流量模式（`mock/lib/stream_scheduler.hpp`）：

```
streams:
  bookTicker  per symbol: every bookticker_us (默认 100µs),  ~150 B
  depth       per symbol: every depth_ms (默认 10 ms),       depth_bytes (默认 1024 B)
  trade       per symbol: Poisson(mean=trade_mean_ms 5ms),   ~200 B
  kline_1m    per symbol: every kline_s (默认 1s),            ~300 B
```

调度实现：
- 单线程主循环维护一个最小堆（`std::priority_queue`，按 next_fire_tsc 排序）
- 每次 loop iteration 先 `recv(MSG_DONTWAIT)` 一次，处理 client order（如有）
- 然后 peek 堆顶：若 `now_tsc >= top.next_fire_tsc`，pop → push 该 stream 一帧 → 计算下一次 fire（周期或 Poisson）→ push 回堆
- 否则继续 spin

`stream_scheduler.hpp` 是 header-only 工具，泛型化（按 stream id + payload generator + interval policy 参数化）。

### 关键代码不在 mock 之间复制

- WS 握手 / 帧编解码：`mock/lib/{ws_handshake.hpp, ws_frame.hpp}`
- TCP/UDP socket 绑定：`mock/lib/{tcp_bind.hpp, udp_bind.hpp}`
- busy-poll 主循环骨架：`mock/lib/busy_poll.hpp`（模板化，注入 RX handler 和 schedule callback）
- `work_spin(ns)`：`mock/lib/work_spin.hpp`（pause-loop 直到 TSC 走过 N ns）

---

## 实施计划

> **Commit 策略**：每个阶段完成并通过验收后，调用 `/git` 提交。commit message 标注阶段编号（如 `bench-rewrite: 完成阶段 2 — core 与 mock/lib 共享层`）。每个阶段是独立回滚点。

### 阶段 1：清空 + 骨架

**目标**：删除所有旧代码，建好新目录骨架，xmake 编译通过（空 target list）。

- 删除：`benchmarks/latency/` 整个目录、`benchmarks/bench_common.hpp`、`scripts/bench_latency.sh`
- 删除：`xmake.lua` 中 `bench_mock_server` / `bench_udp_echo_server` / `bench_tcp_echo_server` 三个 target + `bench_latency` 表驱动循环（覆盖 `udp_echo` / `tcp_echo` / `udp_relay` / `ws_echo` / `market_rx` / `order_rtt`）
- 创建空目录：`benchmarks/latency/{core, mock/lib, tcp, udp, ws, exchange}`
- `xmake.lua` 加新的 latency benchmark section 头部（暂无 target）

**交付物**：空骨架目录 + 干净 xmake.lua

**验收标准**：
- `xmake build` 通过（无新 target，但确认旧 target 已全删）
- `find benchmarks/latency -type f` 仅返回空目录或占位 `.gitkeep`
- git diff 显示净删除约 2500 行旧代码
- `grep -rn "bench_common\|bench_latency\|bench_mock_server\|bench_udp_echo_server" .` 在源码内零结果

**推荐 skill**：`/refactor breaking`（破坏性删除）

---

### 阶段 2：core 与 mock/lib 共享层

**目标**：实现所有共享 header，单测通过。

实现文件：
- `core/{config.hpp, sample.hpp, runner.hpp, scenario_concept.hpp, hist_report.hpp, cpu_pin.hpp, signal.hpp, tsc_protocol.hpp, timer.hpp}`
- `mock/lib/{busy_poll.hpp, tcp_bind.hpp, udp_bind.hpp, ws_handshake.hpp, ws_frame.hpp, work_spin.hpp, stream_scheduler.hpp}`

**单测**（`tests/unit/bench/`）：
- `test_cpu_pin.cpp`：`pin_thread_strict` 在 isolcpus 上 succeed、在非 isolcpus 上 fail；sibling 冲突 fail；NUMA 冲突 fail
- `test_tsc_protocol.cpp`：binary stamp/parse 往返；JSON `parse_T` / `parse_T_recv` / `parse_T_send` 正确性 + 边界
- `test_work_spin.cpp`：`work_spin(1000)` 实测耗时在 [900, 1100] ns 内
- `test_stream_scheduler.cpp`：4 个 stream 1 秒内的发包数量符合预期周期
- `test_ws_frame.cpp`：build / parse 单帧、≤125 / 126–65535 两种长度编码

**交付物**：可编译的 header-only library + 16+ 单测

**验收标准**：
- `xmake test` 全部通过
- `pin_thread_strict` 在非 isolated core 上 fail（除非 `--allow-non-isolated`）
- `work_spin` 实际耗时偏差 ≤ 10%
- `stream_scheduler` 1 秒内 bookTicker 帧数 ≈ 1e6 / bookticker_us（默认 10000 帧）

**推荐 skill**：`/design`

---

### 阶段 3：3 个普通场景（tcp / udp / ws）

**目标**：3 个普通场景的 mock + bench 全部跑通，kernel + DPDK 双 build 都通过。

实现文件：
- `tcp/{mock.cpp, bench.cpp, scenario.hpp}` → 二进制 `mock_tcp` / `mock_tcp_dpdk` / `bench_tcp` / `bench_tcp_dpdk`
- `udp/{mock.cpp, bench.cpp, scenario.hpp}` → 同上
- `ws/{mock.cpp, bench.cpp, scenario.hpp}` → 同上

`xmake.lua` 加 12 个 target（3 mock × 2 build + 3 bench × 2 build）。

**关键实现**：
- `TcpRttScenario` / `UdpRttScenario` / `WsRttScenario` 都实现 `RttScenario` concept
- 二进制协议帧用 `tsc_protocol::stamp_binary` / `parse_binary`
- `bench.cpp` main 流程：`parse_common → tsc::init → pin_strict(client_cpu) → eph::net::*Transport::connect → BenchRunner::run_rtt_sweep`

**集成 smoke test**（手动）：
```bash
# 启动 mock
./build/.../mock_tcp --bind-ip 127.0.0.1 --port 9000 --mock-cpu 4 --allow-non-isolated &
# 跑 bench
./build/.../bench_tcp --server-ip 127.0.0.1 --port 9000 --client-cpu 2 \
    --duration 5 --payload-sizes 64,1024 --allow-non-isolated
```

**验收标准**：
- 12 个 target 全部 build 通过
- 3 个场景 smoke test 输出 4-leg 报告（RTT / TX / RX / Server-leg）p50/p99/p999/max 都非零
- `--server-work-ns 1000` 时 server-leg p50 增加 ≈ 1µs
- `--allow-non-isolated` 关掉时（即 `pin_thread_strict` 强校验）在隔离机器上正常退出 0

**推荐 skill**：`/design`

---

### 阶段 4：3 个量化场景（exchange/market / order / md_udp）

**目标**：3 个量化场景全部跑通，包含多 stream mock 调度与 N-inflight order pipeline。

实现文件：
- `exchange/scenario.hpp`：`MarketRxScenario`（`OneWayScenario`）/ `OrderRttScenario`（`RttScenario`）/ `MdUdpScenario`（`RttScenario`）
- `exchange/mock_ws.cpp` → `mock_exchange_ws` / `mock_exchange_ws_dpdk`
- `exchange/mock_md_udp.cpp` → `mock_exchange_md_udp` / `mock_exchange_md_udp_dpdk`
- `exchange/bench_market.cpp` → `bench_exchange_market` / `bench_exchange_market_dpdk`
- `exchange/bench_order.cpp` → `bench_exchange_order` / `bench_exchange_order_dpdk`
- `exchange/bench_md_udp.cpp` → `bench_exchange_md_udp` / `bench_exchange_md_udp_dpdk`

`xmake.lua` 加 10 个 target（2 mock × 2 build + 3 bench × 2 build）。

**关键实现**：
1. `mock_exchange_ws`：
   - 用 `stream_scheduler` 维护 4 类 stream × N 个 symbol 的优先队列
   - `recv` 路径检测 order JSON → spin work_ns → 回 ExecutionReport（含 T_recv / T_send）
   - bookTicker push 周期默认 100µs
2. `OrderRttScenario`：
   - in-flight order id → send_tsc 哈希表（`std::array<uint64_t, kMaxInflight>` + 单调 id 索引，避免堆分配）
   - 每 `do_one_rtt` 发一个新 order，poll 直到收到匹配 ExecutionReport（用 order id 匹配）
   - `--inflights N` 控制窗口大小：当 in-flight 数 < N 时继续发，否则等响应
3. `MarketRxScenario`：
   - `prepare()` 注册 transport on_message handler，filter `bookTicker` event type
   - `do_one_recv` 在 handler 内 parse `T` field → fill `OneWaySample{producer_tsc=T, consumer_tsc=now}`
4. `MdUdpScenario`：
   - 与 `UdpRttScenario` 同结构，仅 payload 内容是行情格式（`mock/lib` 复用 `udp_bind`）

**集成 smoke test**：
```bash
# market RX
./build/.../mock_exchange_ws --bind-ip 127.0.0.1 --port 9001 --mock-cpu 4 \
    --bookticker-us 100 --allow-non-isolated &
./build/.../bench_exchange_market --server-ip 127.0.0.1 --port 9001 \
    --client-cpu 2 --duration 5 --allow-non-isolated

# order RTT
./build/.../bench_exchange_order --server-ip 127.0.0.1 --port 9001 \
    --client-cpu 2 --duration 5 --inflights 1,16 --allow-non-isolated
```

**验收标准**：
- 10 个 target 全部 build 通过
- `bench_exchange_market` 输出 OneWay 报告，p50 < 100µs（loopback 上）
- `bench_exchange_order --inflights 1,16` 输出两组 RTT 数据，inflight=16 的 p99 严格大于 inflight=1 的 p99
- `bench_exchange_md_udp --payload-sizes 64,1024` 输出 2 组数据
- `mock_exchange_ws` 在 client connect 后 1 秒内推出 ≈ 30000 个 bookTicker 帧（3 symbol × 10kHz）

**推荐 skill**：`/design`

---

### 阶段 5：bench_latency.sh 重写

**目标**：薄壳脚本编排 6 场景 × 2 transport，**不写 `.bench/`**。

新脚本能力：
- preflight：root 权限、NIC 存在、isolcpus 检查、连通性 ping
- netns 切换（kernel 模式：把 NIC-B 移入 `bench_ns`、配置 IP / 路由）
- DPDK 切换（DPDK 模式：调用 `dpdk-setup.sh` bind vfio-pci）
- 每场景启动对应 mock → 跑 client → 停 mock → sleep 0.3s
- `--scenarios` / `--transports` 过滤
- `--dry-run` / `--verbose`
- 退出前复原 NIC（trap on EXIT）
- **所有 bench 输出走 stdout**；用户用 `... | tee my-run.log` 自行落盘

不再有：
- `--output-dir`、`record_env_metadata`、`generate_summary`、JSONL 写入
- `BENCH_OUTPUT_DIR` env

**交付物**：新 `scripts/bench_latency.sh`（预期 ≤ 400 行，旧版 ~700 行）

**验收标准**：
- `sudo ./scripts/bench_latency.sh --nic-a ens34 --nic-b ens35 --server-ip 172.31.21.173 --gateway-ip 172.31.16.1` 一条命令跑完 6 × 2 = 12 组 bench
- `--dry-run` 输出每条会执行的命令、不实际跑
- `--scenarios tcp --transports kernel` 单条 smoke test 通过
- 中途 SIGINT 干净退出，NIC 复原（无 `bench_ns` 残留）
- 脚本不创建任何文件（包括 `.bench/`、`mock.log`）

**推荐 skill**：`/script`

---

### 阶段 6：回归对比 + memory 清理

**目标**：与重写前的最近一次 bench 对比；清理过期 memory。

操作：
1. 在同一台机器上跑 `sudo ./scripts/bench_latency.sh ... --duration 10 | tee /tmp/rewrite-bench.log`
2. 找出重写前最近一次 `.bench/` 中的对应数据（若存在）做并列对比
3. 关键 leg（DPDK 场景的 RTT p50/p99）的差值若超过 ±10%：分析原因并决定（修复 vs 接受）
4. 更新 memory：
   - `feedback_bench_data_convention.md` → 改为"bench 不再写 `.bench/`，stdout 直出，用户自行 `tee`"或删除
   - `project_bench_rewrite.md` → 标记为已完成（"已完成于 2026-04-09，参见 plan-bench-latency-rewrite-20260409-023700.md"）
5. 删除 `.artifacts/bench-*` 旧目录中已无意义的对比基线（可选）

**验收标准**：
- 重写后 DPDK p50/p99 与重写前差值在 ±10% 内（或有书面解释）
- memory 已更新，不再有指向 `.bench/HISTORY.md` 的 stale 记录
- 整个 6 阶段对应 6 个独立 commit，可任意点回滚

**推荐 skill**：`/bench compare` + 手动 memory 清理

---

## 关键决策记录

### D-1: 全删 vs 增量重构
- **问题**：旧代码是删光重写还是保留 framework 层
- **选项**：A 全删、B 留 framework、C 留 framework 全部
- **决策**：A
- **理由**：framework 内工具大多很薄，新设计里 `BenchConfig` 和 ScenarioLike 都要重做，半保留只会让新旧约定打架
- **验收**：阶段 1 完成后 `find benchmarks/latency -type f` 仅占位文件

### D-2: mock 用裸 socket，client 用 eph-net
- **问题**：mock 和 client 是否都走 eph-net
- **选项**：A 全 eph-net、B 混合、C 全裸
- **决策**：B（mock 裸、client eph-net）
- **理由**：mock 必须 busy-poll 0 µs 抖动作为基准，eph-net 事件循环会污染基线；client 需要测产品代码
- **验收**：`mock/lib/` 不依赖 `eph-net`、`scenario/` 全部依赖 `eph-net`

### D-5: server 业务处理用 work_spin
- **问题**："普通业务场景" mock 的语义
- **选项**：A 纯 echo、B echo + spin work_ns、C 真实 hash/查表、D 协议感知
- **决策**：B
- **理由**：A 让 server-leg 永远 ≈ 0；C/D 太重淹没网络抖动；B 是干净的中间点，`--server-work-ns 0` 退化为纯 echo
- **验收**：`--server-work-ns 1000` 时 server-leg p50 增加 ≈ 1µs

### D-6: 加密交易所 mock 流量模型
- **问题**：交易所 WS mock 的"真实流量模式"具体长什么样
- **选项**：A 简单轮播、B 多 stream 加权、C trace 回放、D B + 可选 C
- **决策**：D（初版只做 B，C 留接口位）
- **理由**：B 已能复现关键特性；C 需抓包基础设施，初版不引入；留接口位避免未来重写
- **验收**：`mock_exchange_ws` 实现 4 类 stream（bookTicker/depth/trade/kline）独立周期调度；`--trace FILE` 接受参数但返回 not-implemented

### D-8: 两个 Scenario concept
- **问题**：1-leg 和 RTT 场景是否共用一个 concept
- **选项**：A 共用 + payload=0 hack、B 拆两个 concept、C 共用 + 运行时 leg mask
- **决策**：B
- **理由**：A 是当前痛点（client_send_tsc=t_send 反直觉）；C 破坏 hot-path 内联；B 语义清晰且代码量与 A 相当
- **验收**：`runner.hpp` 提供 `RttScenario` 和 `OneWayScenario` 两个 concept + 三个 sweep 入口

### D-11: order 场景统一 N-inflight
- **问题**：order RTT 是同步还是并发
- **选项**：A 同步、B 并发、C 同步 + 可选并发
- **决策**：B（统一 N-inflight，N=1 即同步基线）
- **理由**：单一代码路径覆盖两种场景，避免维护两份代码
- **验收**：`bench_exchange_order --inflights 1,16` 输出两组数据，inflight=16 的 p99 > inflight=1 的 p99

### D-13: 强校验绑核
- **问题**：绑核检查的严格程度
- **选项**：A 强校验（fail）、B 软校验（warn）、C 不校验
- **决策**：A
- **理由**：bench 数据可信度依赖绑核正确性；用户硬要求"必须绑核"等价于"绑核错了应该 fail"
- **验收**：在非 isolated core 上启动 bench 直接 exit 1（除非 `--allow-non-isolated`）

### D-15: 不写文件
- **问题**：bench 数据是否落盘
- **选项**：A 双写 JSONL + HISTORY.md、B 只 HISTORY.md、C 只 JSONL、D 不写
- **决策**：D（推翻 memory 中"bench data persistence convention"约定）
- **理由**：用户明确表态"如果想落盘就 tee stdout"；简化代码、消除 `JsonlWriter` 整层
- **验收**：bench 进程不创建任何文件；阶段 6 同步更新 memory

---

## 备注：不在代码内但影响实施

- **memory 更新**（阶段 6 必做）：`feedback_bench_data_convention.md` 现在与代码冲突，必须改为"不写文件"或删除
- **未实现接口位**：`mock_exchange_ws --trace FILE` 接受参数但返回 `spdlog::error("trace mode not implemented") + return 1`
- **`stream_scheduler.hpp`** 是 plan 阶段唯一识别出的"非平凡新算法"，单测必须覆盖时序正确性
- **`OrderRttScenario` 的 in-flight hashmap**：用 `std::array<uint64_t, kMaxInflight>` + 自增 id 模数索引，零堆分配
- **HT sibling 检查的实现**：`pin_thread_strict` 需要在进程内维护"已绑核线程列表"（thread-safe），单元测试要覆盖多线程路径
