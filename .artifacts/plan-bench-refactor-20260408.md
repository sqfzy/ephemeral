# Plan: benchmarks/latency 一步到位重构

> 消灭场景间代码重复，统一 raw/WS 场景到单一 BenchRunner，修复所有已知设计缺陷

创建时间：2026-04-08
状态：已确认
前置：
- `.artifacts/plan-bench-rewrite-20260408.md`（首次重写计划）
- `.artifacts/fix-20260408-bench-anomalies.md`（数据修复报告）

---

## 定位与边界

**目标**：在不改变 6 场景外部行为和 CLI 接口的前提下，将每个场景文件从 ~330 行压缩到 ~80 行，消除框架与场景的所有重复代码，并通过统一抽象修复现有的 4 类隐藏问题。

**用户**：bench 维护者 + 添加新场景的开发者

**In scope**
- 重构 6 个 scenario 主文件，从 ~330 行降到 ~80 行
- 提取 framework 子目录：BenchRunner、Stats、Timer、TSC、CLI、DPDK setup、WS transport
- 统一 raw 与 WS 场景到 **同一个 BenchRunner 模板**
- 全部场景采用同步 send→wait→record 模型（彻底消除 in-flight 错配）
- 引入 **per-payload pre-warmup** 阶段（解决 cold start tail）
- 改用 `HdrHistogram::reset()` 复用 histogram 对象，消除每 payload 的 4 次 allocation
- 修复 BenchConfig CLI 解析的 i=0 fragility（通过显式 `from_argv()` 工厂表示）
- 统一 mock 接口和 lifecycle pattern
- 删除每个场景重复的 `parse_json_tsc` / `tsc_to_ns` / DPDK arg-split / `make_bench_tc` 等

**Out of scope**
- 修改 CLI 参数语义（保持向后兼容）
- 修改 JSONL 输出 schema
- 修改 mock server 的协议（TSC 时间戳格式不变）
- 添加新场景或新 transport
- 修改 xmake.lua target 命名（保留 `bench_<scenario>` 和 `bench_<scenario>_dpdk`）

---

## 技术选型

| 类别 | 选择 | 理由 |
|------|------|------|
| 语言 | C++23 | 沿用项目标准；用 concepts、CTAD、designated initializers |
| 抽象方式 | 模板 + concepts（编译期 dispatch） | hot loop 零开销；零虚函数；编译器完整内联 |
| 框架风格 | Header-only | 与 mock 一致，零链接开销，模板友好 |
| 日志 | spdlog（cold path） | 沿用项目标准 |
| 测量 | HdrHistogram + TSC（已有） | 不变 |
| 构建 | xmake table-driven targets（已有） | 不变 |
| 测试 | bench_latency.sh 端到端验证 | 行为一致性 = 关键测试 |

---

## 架构设计

### 文件结构

```
benchmarks/latency/
├── framework/                    ← 新目录
│   ├── bench_config.hpp          — BenchConfig + CLI 解析（修复 i=0 问题）
│   ├── bench_stats.hpp           — LegStats / BenchResult / compute / print / JsonlWriter
│   ├── bench_timer.hpp           — BenchTimer（warmup + duration 状态机）
│   ├── bench_runner.hpp          — BenchRunner<Scenario>（核心抽象）
│   ├── tsc_protocol.hpp          — Binary 与 JSON TSC 字段读写
│   ├── signal.hpp                — g_running + install_signal_handlers + pin_or_die
│   ├── dpdk_setup.hpp            — DpdkBenchEnv: EalGuard + Platform + ARP + 参数解析
│   ├── ws_transport.hpp          — make_bench_ws_transport()（kernel + DPDK 双路径）
│   └── pre_warmup.hpp            — pre_warmup() helper（运行 N 个 dummy round-trip）
├── mock/
│   ├── mock_handle.hpp           — 抽到这里（从 bench_loop.hpp）
│   ├── ws_server.hpp             — rename from mock_ws_server.hpp
│   ├── ws_handshake.hpp          — 不变
│   ├── data_gen.hpp              — 不变
│   ├── tcp_echo_server.hpp       — 不变
│   ├── udp_echo_server.hpp       — 不变
│   └── udp_relay_server.hpp      — 不变
├── scenario/                     ← 新目录
│   ├── tcp_echo.hpp              — TcpEchoScenario<Transport>（do_one_round 实现）
│   ├── udp_echo.hpp              — UdpEchoScenario<Sender>
│   ├── udp_relay.hpp             — UdpRelayScenario<Sender>
│   ├── ws_echo.hpp               — WsEchoScenario<Transport>
│   ├── market_rx.hpp             — MarketRxScenario<Transport>
│   └── order_rtt.hpp             — OrderRttScenario<Transport>
├── bench_tcp_echo.cpp            — ~80 行 thin main
├── bench_udp_echo.cpp            — ~80 行
├── bench_udp_relay.cpp           — ~80 行
├── bench_ws_echo.cpp             — ~80 行
├── bench_market_rx.cpp           — ~80 行
├── bench_order_rtt.cpp           — ~80 行
├── bench_tcp_echo_server.cpp     — 保留（standalone wrapper）
├── bench_udp_echo_server.cpp     — 保留
├── bench_udp_relay_server.cpp    — 保留
└── bench_mock_server.cpp         — 保留

DELETE: bench_loop.hpp            — 内容拆分到 framework/*.hpp
```

### 核心抽象

#### Scenario concept（编译期接口）

每个场景是一个 struct，必须实现：

```cpp
template <typename T>
concept Scenario = requires(T& s, size_t payload, RoundTrip& rt) {
    { s.prepare(payload) } -> std::same_as<bool>;     // resize buffers, etc.
    { s.do_one_round(rt) } -> std::same_as<bool>;     // one send→recv cycle
    { s.cleanup() } -> std::same_as<void>;            // close per-payload resources
    typename T::TransportType;                        // for runner template inference
};
```

`do_one_round(RoundTrip&)` 填充 4 个 TSC 字段，返回 true 表示成功（false 表示超时/错误，本轮跳过）。

#### RoundTrip（一次往返的原始测量）

```cpp
struct RoundTrip {
    uint64_t client_send_tsc = 0;
    uint64_t server_recv_tsc = 0;  // 0 = 不可用（场景无此字段）
    uint64_t server_send_tsc = 0;
    uint64_t client_recv_tsc = 0;
};
```

#### BenchRunner（统一 raw + WS 场景）

```cpp
template <Scenario S>
class BenchRunner {
public:
    BenchRunner(const BenchConfig& cfg, std::string_view scenario_name,
                std::string_view transport_name, JsonlWriter& jsonl);

    /// Run a payload sweep. For each payload size:
    ///   1. scenario.prepare(payload)
    ///   2. pre-warmup (1000 dummy rounds, discarded)
    ///   3. BenchTimer warmup (cfg.warmup seconds, discarded)
    ///   4. BenchTimer measurement (cfg.duration seconds, recorded)
    ///   5. compute + print + write JSONL
    ///   6. histograms.reset() for next iteration
    ///   7. scenario.cleanup()
    void run_sweep(S& scenario, const std::vector<size_t>& payloads);

private:
    void record(const RoundTrip& rt);

    eph::utils::HdrHistogram rtt_, tx_, rx_, srv_;
    const BenchConfig& cfg_;
    std::string_view scenario_name_, transport_name_;
    JsonlWriter& jsonl_;
};
```

注意：histogram 是 BenchRunner 的成员，构造一次，每 payload 调用 `reset()`。

#### DpdkBenchEnv（DPDK 场景的统一 setup）

```cpp
struct DpdkBenchEnv {
    eph::dpdk::EalGuard eal;
    eph::dpdk::Platform platform;
    rte_ether_addr src_mac;
    rte_ether_addr gw_mac;
    uint32_t src_ip;
    uint32_t dst_ip;
    int app_argc;
    char** app_argv;

    /// Factory: parses argc/argv (split at "--"), inits EAL, sets up
    /// platform, resolves ARP. Returns std::expected for error handling.
    static std::expected<DpdkBenchEnv, std::string>
    create(int argc, char** argv, const BenchConfig& cfg);
};
```

每个 DPDK 场景的 main 调用 `DpdkBenchEnv::create()` 一次，~10 行替换之前的 ~50 行 boilerplate。

#### WS Transport 工厂（kernel + DPDK）

```cpp
namespace bench::ws_transport {

#if defined(EPH_USE_DPDK)
using BenchTcpImpl = eph::dpdk::TcpSession<>;
#else
using BenchTcpImpl = eph::net::SocketTransport;
#endif

using BenchWsTransport = eph::net::DirectTransport<BenchTcpImpl, eph::net::WsFramer, 4096>;

/// Build a TransportConfig with bench defaults (no TLS, no pings).
eph::net::TransportConfig make_tc(const BenchConfig& cfg);

#if !defined(EPH_USE_DPDK)
/// Kernel: factory + DirectTransport::create()
std::expected<std::unique_ptr<BenchWsTransport>, std::string>
connect_kernel(const BenchConfig& cfg);
#else
/// DPDK: uses DpdkBenchEnv to call eph::dpdk::connect<BenchWsTransport>()
std::expected<DpdkConnection<BenchWsTransport>, std::string>
connect_dpdk(const BenchConfig& cfg, const DpdkBenchEnv& env);
#endif

} // namespace bench::ws_transport
```

每个 WS 场景的 main 调用 `connect_kernel()` / `connect_dpdk()` 一次，得到 transport 后传给 scenario 构造函数。

### 数据流

```
main()
  ├─ install_signal_handlers()
  ├─ TSC::init()
  ├─ #ifdef DPDK: DpdkBenchEnv::create(argc, argv, cfg)
  │  └─ EalGuard + Platform + ARP + arg-split (一次性)
  ├─ parse_bench_config(app_argc, app_argv) — i=0 起步（修复版）
  ├─ Mock startup (in-process for DPDK; external for kernel)
  ├─ Scenario构造(transport / sockets / sender)
  ├─ JsonlWriter jsonl(cfg.output_path)
  ├─ BenchRunner<Scenario> runner(cfg, name, transport, jsonl)
  └─ runner.run_sweep(scenario, payload_sizes)
       └─ for each payload:
            ├─ scenario.prepare(payload)
            ├─ pre-warmup loop (1000 dummies, discard)
            ├─ BenchTimer.start(warmup, duration)
            ├─ while (timer.is_running()):
            │    ├─ scenario.do_one_round(rt)
            │    ├─ if !timer.is_warmup(): record(rt)
            │    └─ if !rt.valid: continue
            ├─ compute + print + jsonl.write
            ├─ reset histograms
            └─ scenario.cleanup()
```

---

## 接口设计

### bench_config.hpp（修复 i=0 fragility）

```cpp
struct BenchConfig { /* fields unchanged */ };

/// Parse CLI args. Accepts argv from any starting position — the loop
/// starts at i=0 and skips entries that don't match a "--xxx" pattern,
/// so passing argv directly from main() (where argv[0] is program name)
/// is safe, as is passing an offset slice (DPDK app_argv post "--").
BenchConfig parse_bench_config(int argc, char** argv) noexcept;

/// Default payload sizes per scenario type
inline const std::vector<size_t> kTcpPayloads = {64, 128, 256, 512, 1024, 1460};
inline const std::vector<size_t> kUdpPayloads = {64, 128, 512, 1024, 1472};
inline const std::vector<size_t> kWsPayloads  = {64, 128, 256, 512, 1024};
```

### bench_runner.hpp（核心）

```cpp
template <typename Scenario>
class BenchRunner {
public:
    BenchRunner(const BenchConfig& cfg,
                std::string_view scenario_name,
                std::string_view transport_name,
                JsonlWriter& jsonl)
        : cfg_(cfg), scenario_name_(scenario_name),
          transport_name_(transport_name), jsonl_(jsonl),
          rtt_{kHistMin, kHistMax, 3}, tx_{kHistMin, kHistMax, 3},
          rx_{kHistMin, kHistMax, 3}, srv_{kHistMin, kHistMax, 3} {}

    void run_sweep(Scenario& scenario, const std::vector<size_t>& payloads) {
        for (size_t payload : payloads) {
            if (!g_running.load(std::memory_order_relaxed)) break;
            run_one_payload(scenario, payload);
        }
    }

private:
    void run_one_payload(Scenario& scenario, size_t payload) {
        if (!scenario.prepare(payload)) {
            spdlog::error("{}: prepare({}) failed", scenario_name_, payload);
            return;
        }

        spdlog::info("{} ({}): payload={}B, pre_warmup={}, warmup={}s, duration={}s",
                     scenario_name_, transport_name_, payload,
                     kPreWarmupRounds, cfg_.warmup.count(), cfg_.duration.count());

        // Pre-warmup: discard fixed N rounds to prime caches/routes/scheduler
        RoundTrip rt;
        for (size_t i = 0; i < kPreWarmupRounds && g_running.load(); ++i) {
            (void)scenario.do_one_round(rt);
        }

        // BenchTimer-controlled warmup + measure
        BenchTimer timer;
        timer.start(cfg_.warmup, cfg_.duration);

        while (timer.is_running() && g_running.load(std::memory_order_relaxed)) {
            if (!scenario.do_one_round(rt)) continue;
            if (timer.is_warmup()) continue;
            record(rt);
        }

        // Report and reset
        BenchResult result{compute_stats(rtt_), compute_stats(tx_),
                           compute_stats(rx_), compute_stats(srv_)};
        print_bench_result(scenario_name_.data(), payload, result);
        jsonl_.write(scenario_name_, transport_name_, payload, result);

        rtt_.reset(); tx_.reset(); rx_.reset(); srv_.reset();
        scenario.cleanup();
    }

    void record(const RoundTrip& rt) {
        if (rt.client_recv_tsc > rt.client_send_tsc) {
            [[maybe_unused]] auto _ = rtt_.record(
                tsc_to_ns(rt.client_recv_tsc - rt.client_send_tsc));
        }
        if (rt.server_recv_tsc > 0 && rt.server_recv_tsc > rt.client_send_tsc) {
            [[maybe_unused]] auto _ = tx_.record(
                tsc_to_ns(rt.server_recv_tsc - rt.client_send_tsc));
        }
        if (rt.server_send_tsc > 0 && rt.client_recv_tsc > rt.server_send_tsc) {
            [[maybe_unused]] auto _ = rx_.record(
                tsc_to_ns(rt.client_recv_tsc - rt.server_send_tsc));
        }
        if (rt.server_recv_tsc > 0 && rt.server_send_tsc > rt.server_recv_tsc) {
            [[maybe_unused]] auto _ = srv_.record(
                tsc_to_ns(rt.server_send_tsc - rt.server_recv_tsc));
        }
    }

    static constexpr uint64_t kHistMin = 10;
    static constexpr uint64_t kHistMax = 1'000'000'000ULL;
    static constexpr size_t kPreWarmupRounds = 2000;

    const BenchConfig& cfg_;
    std::string_view scenario_name_, transport_name_;
    JsonlWriter& jsonl_;
    eph::utils::HdrHistogram rtt_, tx_, rx_, srv_;
};
```

### scenario/udp_echo.hpp（示例：典型 raw scenario）

```cpp
template <typename Sender>
class UdpEchoScenario {
public:
    using TransportType = Sender;

    UdpEchoScenario(Sender& sender, RecvFn recv) /* ... */;

    bool prepare(size_t payload) {
        if (payload < 24) return false;
        send_buf_.assign(payload, 0xAB);
        return true;
    }

    bool do_one_round(RoundTrip& rt) {
        rt.client_send_tsc = eph::utils::TSC::now();
        std::memcpy(send_buf_.data(), &rt.client_send_tsc, 8);

        if (!sender_.send(send_buf_.data(), send_buf_.size())) {
            return false;
        }

        if (!recv_(recv_buf_, sizeof(recv_buf_))) {
            return false;
        }

        rt.client_recv_tsc = eph::utils::TSC::now();
        std::memcpy(&rt.server_recv_tsc, recv_buf_ + 8, 8);
        std::memcpy(&rt.server_send_tsc, recv_buf_ + 16, 8);
        return true;
    }

    void cleanup() {} // nothing to do per-payload

private:
    Sender& sender_;
    RecvFn recv_;
    std::vector<uint8_t> send_buf_;
    uint8_t recv_buf_[2048];
};
```

### scenario/ws_echo.hpp（同步 WS 场景）

```cpp
template <typename Transport>
class WsEchoScenario {
public:
    using TransportType = Transport;

    explicit WsEchoScenario(Transport& transport) : transport_(transport) {
        // Set on_message ONCE in ctor
        auto& tc = const_cast<eph::net::TransportConfig&>(transport_.config());
        tc.on_message = [this](const uint8_t* data, uint16_t len, uint8_t) {
            std::string_view json(reinterpret_cast<const char*>(data), len);
            if (json.find("\"echo\":true") == std::string_view::npos) return;
            current_.client_recv_tsc = eph::utils::TSC::now();
            current_.server_send_tsc = tsc::parse_T(data, len);
            current_.server_recv_tsc = tsc::parse_T_recv(data, len);
            got_response_ = true;
        };
    }

    bool prepare(size_t payload) {
        padding_.assign(payload > 48 ? payload - 48 : 1, 'X');
        return true;
    }

    bool do_one_round(RoundTrip& rt) {
        got_response_ = false;
        rt.client_send_tsc = eph::utils::TSC::now();

        char buf[2048];
        int n = std::snprintf(buf, sizeof(buf),
            R"({"d":"%.*s","T_send":%llu})",
            static_cast<int>(padding_.size()), padding_.c_str(),
            static_cast<unsigned long long>(rt.client_send_tsc));
        [[maybe_unused]] auto err = transport_.send_text(
            std::string_view(buf, static_cast<size_t>(n)));

        // Sync: poll until response (or shutdown)
        while (!got_response_ && g_running.load(std::memory_order_relaxed)
               && transport_.is_running()) {
            [[maybe_unused]] auto _ = transport_.poll();
        }
        if (!got_response_) return false;

        rt = current_;  // current_.client_send_tsc was set above
        rt.client_send_tsc = current_.client_send_tsc;
        return true;
    }

    void cleanup() {}

private:
    Transport& transport_;
    std::string padding_;
    bool got_response_ = false;
    RoundTrip current_;
};
```

注：WS scenario 的 `current_` struct 在 callback 中填充，`do_one_round` 返回时拷贝到 caller 的 `rt`。

### bench_udp_echo.cpp（重构后的 thin main，目标 ~80 行）

```cpp
/// @file bench_udp_echo.cpp
/// Raw UDP echo latency benchmark — sync send-recv with TSC stamps.
///
/// Compiled twice by xmake: bench_udp_echo / bench_udp_echo_dpdk.

#include "framework/bench_config.hpp"
#include "framework/bench_runner.hpp"
#include "framework/bench_stats.hpp"
#include "framework/dpdk_setup.hpp"
#include "framework/signal.hpp"
#include "scenario/udp_echo.hpp"

#if defined(EPH_USE_DPDK)
#include "framework/dpdk_setup.hpp"
#include "mock/udp_echo_server.hpp"
#else
#include "framework/kernel_udp_socket.hpp"  // tiny helper
#endif

int main(int argc, char** argv) {
    bench::install_signal_handlers();
    if (!eph::utils::TSC::init()) { spdlog::error("TSC init failed"); return 1; }

#if defined(EPH_USE_DPDK)
    auto env = bench::DpdkBenchEnv::create(argc, argv, /*needs ws*/false);
    if (!env) { spdlog::error("{}", env.error()); return 1; }
    auto cfg = bench::parse_bench_config(env->app_argc, env->app_argv);
    if (cfg.payload_sizes.empty()) cfg.payload_sizes = bench::kUdpPayloads;
    if (cfg.server_ip.empty() || cfg.local_ip.empty() || cfg.gateway_ip.empty()) {
        spdlog::error("--server-ip, --local-ip, --gateway-ip required");
        return 1;
    }

    auto mock = bench::mock::start_udp_echo({cfg.server_ip, cfg.server_port}, cfg.mock_cpu);
    auto sender = env->make_udp_sender(cfg.server_port);  // helper in DpdkBenchEnv
    if (!sender) { return 1; }

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");
    bench::JsonlWriter jsonl(cfg.output_path);
    bench::scenario::UdpEchoScenario scenario{*sender, env->make_udp_recv()};
    bench::BenchRunner runner{cfg, "udp_echo", "dpdk", jsonl};
    runner.run_sweep(scenario, cfg.payload_sizes);

    bench::stop_mock(mock);
#else
    auto cfg = bench::parse_bench_config(argc, argv);
    if (cfg.payload_sizes.empty()) cfg.payload_sizes = bench::kUdpPayloads;
    if (cfg.server_ip.empty()) { spdlog::error("--server-ip required"); return 1; }

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");
    bench::KernelUdpSocket sock{cfg.server_ip, cfg.server_port};
    bench::JsonlWriter jsonl(cfg.output_path);
    bench::scenario::UdpEchoScenario scenario{sock, sock.make_recv()};
    bench::BenchRunner runner{cfg, "udp_echo", "kernel", jsonl};
    runner.run_sweep(scenario, cfg.payload_sizes);
#endif
    return 0;
}
```

每个场景 main 大致就是这个结构：~80 行，**没有任何 hot loop 代码**，全部 boilerplate 集中在 framework。

---

## 编码规范

| 维度 | 规范 |
|------|------|
| 命名空间 | framework: `bench::`；scenario: `bench::scenario::`；mock: `bench::mock::` |
| 类型命名 | `BenchRunner`、`BenchTimer`、`BenchResult`、`UdpEchoScenario`（驼峰，对外 API） |
| 函数命名 | snake_case（自由函数），驼峰（成员） |
| 文件命名 | `framework/bench_<purpose>.hpp`、`scenario/<scenario_name>.hpp`、`bench_<scenario>.cpp` |
| 错误处理 | factory 用 `std::expected<T, std::string>`；scenario 用 bool（hot path） |
| 日志 | spdlog INFO（per-payload header / report），ERROR（init 失败），DEBUG（每 N 次进度） |
| `[[nodiscard]]` 处理 | 用 `[[maybe_unused]] auto _ = ...` 显式忽略 |
| Hot path 抽象 | 必须模板化或 `inline`；禁止 `std::function` |
| Cold path 抽象 | 接受 polymorphism / `std::function` / 虚函数 |
| 注释 | 每个 framework 头文件顶部 `@file` + 设计意图；非平凡函数标 `@param/@return`；hot path 解释 *why* |

---

## 实施计划

> **Commit 策略**：每个阶段独立 commit，commit message 标注 `refactor(bench): phase N — <title>`。每阶段结束后必须通过 bench_latency.sh 端到端验证（kernel + dpdk）才算通过。
>
> **回滚锚点**：阶段 0 之前的 `7b00bc8` 是已知好状态。任意阶段失败可回退到该 commit。

### 阶段 0: 框架层（无场景改动）

**交付物**：
- `framework/bench_config.hpp`（迁移自 `bench_config.hpp`，修复 i=0 fragility，补 doc）
- `framework/bench_stats.hpp`（迁移 `LegStats` / `BenchResult` / `compute_stats` / `print_*` / `JsonlWriter`）
- `framework/bench_timer.hpp`（迁移 `BenchTimer`）
- `framework/signal.hpp`（迁移 `g_running` / `install_signal_handlers` / `pin_or_die`）
- `framework/tsc_protocol.hpp`（**新建**：从所有 .cpp 收集 `parse_json_tsc` / `parse_T` / `parse_T_recv` / `tsc_to_ns`，去重）
- `framework/bench_runner.hpp`（**新建**：`BenchRunner<Scenario>`、`RoundTrip`、`Scenario` concept）
- `mock/mock_handle.hpp`（迁移自 `bench_loop.hpp`）
- 删除 `bench_loop.hpp`

**验收标准**：
- 所有现有 6 scenarios + 4 mocks **保持不变**仍编译通过（include 路径更新为 framework/）
- bench_latency.sh kernel 模式跑通 udp_echo（最快验证），数据与之前一致

**推荐 skill**：`/refactor`（破坏性接口迁移）

### 阶段 1: DPDK + WS Transport 抽象

**交付物**：
- `framework/dpdk_setup.hpp`（**新建**）：
  - `DpdkBenchEnv` struct + `create(argc, argv, BenchConfig)` factory
  - 封装 EalGuard、Platform、ARP、MAC 解析、arg-split
  - 提供 `make_udp_sender(port)` helper
- `framework/ws_transport.hpp`（**新建**）：
  - `BenchTcpImpl` + `BenchWsTransport` typedefs（编译期 transport 选择）
  - `make_tc(BenchConfig)` 替代每个 .cpp 的 `make_bench_tc()`
  - `connect_kernel(cfg)` 和 `connect_dpdk(cfg, env)` 工厂

**验收标准**：
- 新 framework 头文件编译通过（用一个临时 ad-hoc 测试 .cpp 验证）
- 所有 6 scenarios 仍然可以编译运行（只是还没用到新的）

### 阶段 2: 重构 raw 场景（tcp/udp echo, udp relay）

**交付物**：
- `scenario/tcp_echo.hpp`、`scenario/udp_echo.hpp`、`scenario/udp_relay.hpp`（新建）
- `bench_tcp_echo.cpp`、`bench_udp_echo.cpp`、`bench_udp_relay.cpp`（重写为 thin main，目标每个 ~80 行）
- 删除每个 .cpp 内重复的 `tsc_to_ns`、socket helpers、send_all / recv_exact 等
- send_all / recv_exact / fixed-size send 移到 `framework/tcp_io.hpp` 或场景 header 内

**验收标准**：
- 每个文件 ≤100 行
- bench_latency.sh 跑通 3 个 raw 场景 × kernel/DPDK = 6 个 binary
- 数据与重构前对比：p50 相对偏差 < 5%
- pre-warmup 阶段生效（首 payload p99 不再异常）

### 阶段 3: 重构 WS 场景（ws_echo, market_rx, order_rtt）

**交付物**：
- `scenario/ws_echo.hpp`、`scenario/market_rx.hpp`、`scenario/order_rtt.hpp`
- `bench_ws_echo.cpp`、`bench_market_rx.cpp`、`bench_order_rtt.cpp`（重写）
- order_rtt 同步化：order_interval 不再用作 send pacing，改为 sync send→wait→record loop（同 ws_echo）
- market_rx 保持纯接收（无 send），但用统一 BenchRunner 框架

**验收标准**：
- 每个文件 ≤100 行
- bench_latency.sh 跑通 3 个 WS 场景 × kernel/DPDK = 6 个 binary
- ws_echo RTT 数据与阶段 2 完成后的 commit 一致（`7b00bc8` 已修复版）
- order_rtt 同步化后 RTT 应略高（更紧密的 send-recv 循环 → 实际 RTT 测量）但数据形态合理

### 阶段 4: mock_handle 整理 + ws_server rename + 文档

**交付物**：
- `mock/ws_server.hpp` 从 `mock_ws_server.hpp` 重命名（并更新所有 includer）
- `mock/mock_handle.hpp` 已迁移（阶段 0 完成）
- README 或 inline doc 说明：如何添加新场景（写一个 `scenario/<name>.hpp` 实现 Scenario concept + 写一个 ~80 行 thin main）
- xmake.lua 更新（include path 改为 `benchmarks/latency/framework`，可选）

**验收标准**：
- `bench_latency.sh --transports kernel,dpdk` 全 6 场景跑通
- 数据完整、无回归
- 文件总行数与重构前对比：scenario .cpp 总行数 < 600 行（vs 当前 ~1956 行，70% 减少）
- 所有 commit 历史可独立 rollback

---

## 关键决策记录

### D-1: Scenario 抽象 = 模板 concept，非虚函数
- **问题**：如何在 hot path 中调用 scenario.do_one_round 而不引入虚函数开销？
- **选项**：
  - A. 虚函数 `class Scenario { virtual bool do_one_round(...) = 0; }`
  - B. `std::function<bool(RoundTrip&)>`
  - C. 模板 + concept（编译期 dispatch）
- **决策**：C
- **理由**：A 引入间接调用 + cache miss；B 是 type erasure 也有间接调用 + 可能堆分配；C 编译器可以完整内联 `do_one_round` 进 BenchRunner 的循环。bench 的目的是测量纳秒级延迟，hot path 必须零开销。
- **验收标准**：`objdump -d bench_udp_echo` 中 `BenchRunner::run_sweep` 的循环展开应包含 scenario 代码，无间接调用

### D-2: pre-warmup 在每个 payload 内执行
- **问题**：cold start 在哪一层处理？
- **选项**：
  - A. 全局 pre-warmup（仅在第一个 payload 之前跑一次）
  - B. 每 payload pre-warmup（每个 payload 开始时跑 N 个 dummy）
  - C. 不做 pre-warmup，依靠 BenchTimer 的 1s warmup
- **决策**：B（默认 N=2000 rounds）
- **理由**：每个 payload size 走的代码路径略有不同（buffer size、NIC descriptor 配置）。全局 warmup 只能保证第一个 payload 没问题。每 payload pre-warmup 简单可靠。2000 rounds 在 ~25us RTT 下约 50ms，对总耗时影响可忽略。
- **验收标准**：所有场景的所有 payload size，p99 不应高于 p50 的 3 倍（无 cold start tail）

### D-3: HdrHistogram 在 BenchRunner 里复用，用 reset()
- **问题**：每 payload 创建新 histogram instance，还是复用？
- **选项**：
  - A. 每 payload `new HdrHistogram{...}` （现状）
  - B. 复用，调 `reset()` （HdrHistogram 已支持）
- **决策**：B
- **理由**：减少 5 × 4 = 20 次大对象 alloc/free（每个 histogram ~几 KB～几 MB）；reset 是 O(buckets) 不重新分配；与 framework "long-lived runner" 设计一致
- **验收标准**：BenchRunner 构造时分配一次，run_sweep 全程不再 new histogram

### D-4: order_rtt 改为同步设计
- **问题**：order_rtt 原设计是按 order_interval 持续发送（非同步），是否保留？
- **选项**：
  - A. 保留异步设计（与真实交易系统一致）
  - B. 改为同步（与其他场景一致，简化 framework）
  - C. 异步设计 + 添加 in-flight 队列正确处理 RTT 关联
- **决策**：B
- **理由**：当前 order_interval=1000us ≫ RTT=25us 时异步设计巧合可工作，但用户调小 interval 会立即破坏 RTT 测量。同步化后 order_interval 失去意义（自然变为 RTT 节流），但数据始终正确。bench 的目的是测延迟，不是模拟真实系统的并发模型。
- **验收标准**：order_rtt RTT p50 与重构前 ±5%

### D-5: 文件结构 framework/ + scenario/ 目录化
- **问题**：是否引入 framework/ 和 scenario/ 子目录？前一个 plan 选择了扁平结构。
- **选项**：
  - A. 扁平：`bench_*.hpp`、`bench_*.cpp` 全在 latency/ 根目录
  - B. 目录化：`framework/`、`scenario/`、`mock/` 三个子目录
- **决策**：B
- **理由**：6 + 6 + 8 = 20 个 .hpp 在根目录会乱。目录化后导航成本降低，新手能立即理解三层结构（framework → scenario → main）。第一次 plan 选 A 是因为只有几个文件；现在框架成熟、抽象层次清晰，目录化是更好的最终态。
- **验收标准**：xmake.lua 的 add_includedirs 包含 `benchmarks/latency/framework`、`benchmarks/latency/scenario`、`benchmarks/latency`

### D-6: 不重新设计 CLI，但修复 i=0 fragility
- **问题**：parse_bench_config 的 `i=0` vs `i=1` 是个隐患（DPDK app_argv vs 主 argv 不对称）
- **选项**：
  - A. 接受当前 fix（i=0 + 程序名不会匹配 --xxx）
  - B. 提供两个明确入口：`parse_argv_main(argc, argv)` 跳过 argv[0]，`parse_argv_slice(argc, argv)` 不跳过
- **决策**：A
- **理由**：当前 fix 已经能工作且文档化了 invariant（程序名永远不会匹配 `--xxx`）。引入两个 API 增加复杂度，收益小。`auto` 推荐哲学是"设计最优"而非"接口完美"——这里的设计已经够干净。
- **验收标准**：parse_bench_config 文档字符串说清楚 invariant

---

## 一致性检查

- ✅ Scenario concept 无虚函数 ↔ hot path 零开销目标一致
- ✅ BenchRunner 模板化 ↔ Scenario concept 编译期 dispatch
- ✅ HdrHistogram 在 runner 里复用 ↔ runner 是 long-lived 对象（per main）
- ✅ DpdkBenchEnv 集中所有 DPDK 初始化 ↔ scenario 不需要知道 EAL 细节
- ✅ ws_transport.hpp 提供 typedef ↔ scenario 用 `using TransportType = WsBenchTransport`
- ✅ 同步设计跨所有场景 ↔ BenchRunner 的 `do_one_round` 抽象天然同步
- ✅ pre-warmup 在 BenchRunner 内部 ↔ 不需要每个 scenario 自己实现
- ✅ 命名空间 `bench::` / `bench::scenario::` / `bench::mock::` 三层 ↔ 目录结构对应
- ✅ 实施 4 个阶段都有明确的回滚点 ↔ 阶段间无破坏性 cascading 依赖

---

## 风险与缓解

| 风险 | 缓解策略 |
|------|---------|
| 模板化 BenchRunner 导致编译时间显著增加 | 每个 scenario 只实例化一次（在 main.cpp 中），实际增量小 |
| Scenario concept 约束过严，未来添加 transport 类型困难 | concept 只要求 `prepare/do_one_round/cleanup`，未来扩展加新方法即可 |
| order_rtt 同步化改变了语义，用户脚本可能依赖原行为 | 同步化后 order_interval 参数仍可解析（向后兼容），但实际不影响测量 |
| 阶段 2 重构后 raw 场景数据出现微小回归 | pre-warmup 提供更稳定的基线，应改善而非回归。若回归 > 5% 立即调查 |
| 阶段 3 WS 场景重构破坏 DirectTransport 集成 | DirectTransport API 不变，scenario 只是把已有的逻辑重新组织 |

---

## 完成后预期收益

| 指标 | 当前 | 目标 |
|------|------|------|
| 6 个 scenario .cpp 总行数 | ~1956 行 | < 600 行 (-70%) |
| 单个 scenario .cpp 平均行数 | ~326 行 | ~80 行 |
| 重复的 TSC 解析函数 | 5 份 | 1 份 |
| 重复的 DPDK EAL boilerplate | 4 份 | 1 份（DpdkBenchEnv） |
| 添加新场景需要的代码 | ~300 行 + xmake | ~80 行 + xmake |
| HdrHistogram 每 sweep 分配次数 | 5 × 4 = 20 | 4（runner 构造时） |
| Cold start tail 异常 | 间歇出现 | 消失（pre-warmup） |
| order_rtt async 隐患 | 存在 | 消失（同步化） |
| Hot path 间接调用 | 0（已经模板化） | 0（保持） |
