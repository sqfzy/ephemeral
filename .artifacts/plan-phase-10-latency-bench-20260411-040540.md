# Plan: Phase 10 — benchmarks/latency 全面迁移

> 把 `benchmarks/latency/` 从 Phase 6 的 "简化 demonstrator" 重新对齐到 baseline 的**语义等价**测量，同时用 v3.3 Stream/Poller + 外部 Python mock 的风格简化实现。6 个场景，全部 kernel+DPDK 变体，性能不回退。

创建时间：2026-04-11
状态：已确认
分支：refactor/transport-api
基于 commit：ae8a5dd
上游文档：
- `.artifacts/plan-phase-9-recovery-20260410-180306.md` — Phase 9 的延续哲学
- `.artifacts/design-eph-v3.3-architecture-20260410.md` — v3.3 架构 SSOT
- `.temp/baseline-pre-v3.3/benchmarks/latency/` — 语义参考源

---

## 定位与边界

**目标**：v3.3 的 6 个 end-to-end latency 场景**语义等价于 baseline**（相同 wire 协议 / 相同测量方式 / 相同统计报告），但实现形态完全按 v3.3 风格（Stream/Poller API、外部 mock、bench.conf 单一配置源），且性能不回退。

**用户**：
- HFT 开发者做 pre-release 性能验证
- 性能回归的 smoke 检测
- kernel vs DPDK 公平对比的 source of truth

**In scope**：
- 6 个 scenario binary：`lat_tcp` / `lat_udp` / `lat_ws` / `lat_ex_market` / `lat_ex_order` / `lat_ex_md_udp`
- 每个 scenario 自动产出 `_dpdk` sibling（via xmake auto-glob）
- 6 个对应 Python mock（3 个共享 helper + 6 scenario 专属）
- bench.conf INI section 扩展
- `lat` wrapper 脚本重写（dumb dispatcher + NIC 状态管理）
- 旧 `core/` 头文件清理（5 个删）
- 旧测试清理（5 个删 + 2 个扩展）
- 性能对比（kernel 6 场景 + DPDK best-effort）
- 文档更新（CLAUDE.md、bench.conf 示例）

**Out of scope**：
- payload sweep（不做，单参数运行）
- 4-leg 延迟分解（不做，只测 end-to-end）
- eph-json 依赖（不引入；ex_market 手写 scanner）
- 1 MHz 以上 push rate（Python mock 上限 ~200 kHz）
- Mock 自动 install（socat/websockets 等系统工具不依赖）
- DPDK 物理硬件强制验证（best-effort，跑不了降级 sanity）
- 新增测试 framework / runner 抽象层（不复刻 baseline 的 BenchRunner / scenario_concept）

---

## 技术选型

完全继承 v3.3 + Python stdlib 作为 mock 栈。

| 类别 | 选择 | 理由 |
|---|---|---|
| Client 语言 | C++23 | 继承 v3.3 |
| Client API | `eph::net::kernel/dpdk::TcpStream<...>` + `UdpSocket<...>` + `Poller` | v3.3 原生栈 |
| Codec | `eph::codec::RawStreamCodec` / `WsCodec` / `RawDatagramCodec` / `Mold64Codec` | 已有，无新 codec |
| 测量 clock | `clock_gettime(CLOCK_MONOTONIC_RAW)` | **D-6**：与 Python mock 对齐，invariant-TSC vDSO ~20ns 开销等同 rdtsc |
| 统计 | `eph::utils::Recorder`（raw ns 输入） | **D-2**：复用一致性 > 极简主义 |
| 构建 | xmake (auto-glob `lat_*.cpp`) | 继承 |
| Mock 语言 | Python 3 stdlib | **D-3**：统一栈、零外部依赖、jitter 在 one-way 场景不影响测量 |
| Mock WS 实现 | stdlib `hashlib.sha1` + `base64` + `struct` 手写 RFC 6455 | 无 `websockets` 包依赖 |
| Mock 时钟 | `ctypes.CDLL('libc.so.6').clock_gettime(CLOCK_MONOTONIC_RAW)` via `_clock.py` | 精确 ns 精度 |
| Mock 速率控制 | Busy-loop in `_rate.py`（固定 CPU） | ~100 kHz 极限可保证 |
| bench.conf 格式 | INI (global + `[lat_*]` sections) | 简单，现有 parser 改 50 行即可 |
| lat 脚本 | Bash + `nmcli`/`ip` 命令 | 保持现有 NIC 状态管理习惯 |

---

## 架构设计

### 模块边界

**不新增模块**，只修改 `benchmarks/latency/` 目录。

```
benchmarks/latency/
├── bench.conf                    # ★ 扩展（加 INI sections）
├── lat                           # ★ 重写（dumb dispatcher）
├── xmake.lua                     # 微调（scenario 自动 glob 已有）
│
├── core/                         # ★ 大扫除
│   ├── config.hpp                # 扩展：+ ScenarioConfig
│   ├── signal.hpp                # 保留
│   ├── socket_bind.hpp           # 保留
│   │
│   ├── tsc_protocol.hpp          # ❌ DELETE
│   ├── stream_scheduler.hpp      # ❌ DELETE
│   ├── ws_framing.hpp            # ❌ DELETE
│   └── ws_handshake.hpp          # ❌ DELETE
│
├── mocks/                        # ★ 新目录
│   ├── README.md                 # 说明每个 mock 的职责 + 独立启动方法
│   ├── _clock.py                 # 共享：clock_gettime(CLOCK_MONOTONIC_RAW)
│   ├── _rate.py                  # 共享：busy-loop rate limiter
│   ├── _ws.py                    # 共享：RFC 6455 handshake + framing
│   │
│   ├── tcp_echo.py               # TCP echo server
│   ├── udp_echo.py               # UDP echo server
│   ├── ws_echo.py                # WS echo (imports _ws)
│   ├── ex_order_echo.py          # WS echo (imports _ws, JSON passthrough)
│   ├── ex_market_push.py         # WS push with rate control
│   └── ex_md_udp_push.py         # UDP push with Mold64 format
│
├── tcp/lat_tcp.cpp               # ★ 重写
├── udp/lat_udp.cpp               # ★ 重写
├── ws/lat_ws.cpp                 # ★ 重写
└── exchange/
    ├── lat_ex_market.cpp         # ★ 重写
    ├── lat_ex_order.cpp          # ★ NEW
    └── lat_ex_md_udp.cpp         # ★ NEW
```

**影响的现有文件**（修改，不新模块）：
- `tests/unit/bench/` 下的 5 个旧 test 删除 + 2 个扩展 + 1 个更新

### 数据流

```
bench.conf
   |
   +---> lat script (dumb)
   |       |
   |       +--> check environment (python3)
   |       +--> transition NIC state (kernel | vfio-pci | bench_ns)
   |       +--> spawn mock: python3 benchmarks/latency/mocks/<scenario>.py --config bench.conf
   |       +--> exec client: benchmarks/latency/<dir>/lat_<scenario>[_dpdk] --config bench.conf
   |       +--> wait for client exit
   |       +--> kill mock
   |       +--> restore NIC state
   |
   +---> Python mock (reads global + [lat_<scenario>] section)
   |        |
   |        +--> bind to mock_nic IP:port
   |        +--> for RTT scenarios: echo loop
   |        +--> for one-way scenarios: rate-limited push with T field
   |
   +---> C++ client (reads global + [lat_<scenario>] section)
            |
            +--> create KernelTcpStream<...> / DpdkTcpStream<...> / UdpSocket<...>
            +--> attach to Poller
            +--> measurement loop:
            |      for duration in seconds:
            |        RTT scenarios:
            |          t0 = clock_gettime(CLOCK_MONOTONIC_RAW)
            |          stream->send(...)
            |          poll until on_message
            |          t1 = clock_gettime(CLOCK_MONOTONIC_RAW)
            |          recorder.push(t1 - t0)
            |        One-way scenarios:
            |          poll until on_message / on_datagram
            |          t_recv = clock_gettime(CLOCK_MONOTONIC_RAW)
            |          T_server = extract from payload  # hand-written scanner for JSON; binary offset for Mold64
            |          recorder.push(t_recv - T_server)
            +--> on duration expire or SIGINT:
                   stop loop
                   print report: scenario, cfg, sample_count, min, p50, p99, p99.9, max
```

### 核心抽象

#### ScenarioConfig (新增)

在 `core/config.hpp` 新增：

```cpp
namespace bench {

/// Key-value map parsed from a `[lat_<name>]` INI section.
/// Each scenario interprets the keys it cares about.
class ScenarioConfig {
public:
    /// Load from bench.conf for a specific scenario name.
    /// Returns empty ScenarioConfig if section is missing (caller must check required keys).
    static std::expected<ScenarioConfig, std::string>
    load(std::string_view conf_path, std::string_view scenario_name);

    /// Get a value by key, with default if missing.
    std::string get_string(std::string_view key, std::string_view def = "") const;

    /// Get a value with type conversion. Returns unexpected if present but unparseable.
    std::expected<uint32_t, std::string>    get_u32(std::string_view key) const;
    std::expected<uint32_t, std::string>    get_u32(std::string_view key, uint32_t def) const;
    std::expected<uint64_t, std::string>    get_u64(std::string_view key) const;
    std::expected<uint64_t, std::string>    get_u64(std::string_view key, uint64_t def) const;
    std::expected<double, std::string>      get_double(std::string_view key) const;

    /// Check presence
    bool has(std::string_view key) const;

private:
    std::unordered_map<std::string, std::string> kv_;
};

} // namespace bench
```

Existing `CommonConfig`（global NIC/IP/CPU layout）不变，与 ScenarioConfig 并列。Client 代码加载两个：

```cpp
auto common = bench::CommonConfig::load("bench.conf").value();
auto scenario = bench::ScenarioConfig::load("bench.conf", "lat_tcp").value();

auto port = scenario.get_u32("port").value();
auto payload_size = scenario.get_u32("payload_size", 256);
auto duration = scenario.get_u32("duration_seconds", 10);
```

### bench.conf 扩展（完整示例）

```ini
# Global configuration (no section header)
# NIC topology
mock_nic       = ens34
mock_ip        = 10.0.0.1
client_nic     = ens35
client_ip      = 10.0.0.2
netmask        = 255.255.255.0

# CPU pinning
cpu_client     = 4
cpu_mock       = 6

# Common measurement parameters
warmup_samples = 1000           # samples discarded before measurement

[lat_tcp]
port            = 20000
payload_size    = 256
duration_seconds = 10

[lat_udp]
port            = 20001
payload_size    = 256
duration_seconds = 10

[lat_ws]
port            = 20002
ws_path         = /echo
payload_size    = 256
duration_seconds = 10

[lat_ex_market]
port             = 20003
ws_path          = /ws/bookticker
push_rate_hz     = 100000       # 100 kHz (Python mock sustained limit)
duration_seconds = 30

[lat_ex_order]
port             = 20004
ws_path          = /ws/order
inflight         = 16            # pipelining depth
order_count      = 10000         # total orders (or hits duration_seconds whichever first)
duration_seconds = 30

[lat_ex_md_udp]
port             = 20005
push_rate_hz     = 100000        # 100 kHz (was 1 MHz in baseline; Python limit)
msg_per_packet   = 5
duration_seconds = 30
```

---

## 接口设计

### lat wrapper CLI

```bash
sudo ./benchmarks/latency/lat <scenario> [options]

SCENARIOS:
  tcp          raw TCP echo RTT
  udp          raw UDP echo RTT
  ws           plain WebSocket echo RTT
  ex_market    exchange one-way market data push (WS)
  ex_order     exchange N-inflight order RTT (WS, JSON)
  ex_md_udp    exchange UDP market data one-way

OPTIONS:
  --dpdk                  Use DPDK client variant (ens35 to vfio-pci)
  --config <path>         Override bench.conf path (default: benchmarks/latency/bench.conf)
  --help                  Show help

EXAMPLES:
  sudo ./benchmarks/latency/lat tcp
  sudo ./benchmarks/latency/lat ws --dpdk
  sudo ./benchmarks/latency/lat ex_market --config my-bench.conf
```

**lat script 职责**（bash，~150 LOC）：
1. Parse argv: scenario + optional --dpdk/--config
2. Check environment:
   - `python3` available
   - `benchmarks/latency/mocks/<scenario>.py` exists
   - Client binary built (`build/linux/x86_64/release/lat_<scenario>[_dpdk]`)
3. Read minimal fields from bench.conf: mock_nic, client_nic, mock_ip, client_ip
4. Transition NIC state:
   - kernel mode: ensure client_nic is up in host kernel namespace
   - DPDK mode: move client_nic to vfio-pci (idempotent — if already in vfio-pci, skip)
   - bench_ns mode (optional): put client_nic in dedicated namespace for kernel bench isolation
5. Assign IPs to mock_nic (ifconfig/ip addr)
6. Fork:
   - child: `exec python3 mocks/<scenario>.py --config <bench.conf>` — child inherits signal handler for clean shutdown
   - parent: wait 200ms for mock ready, then `exec <client binary> --config <bench.conf>`
7. On client exit:
   - SIGTERM to mock, waitpid
   - Restore NIC state (reverse of step 4)
   - Propagate client exit code

### Client API pattern (shared across all 6 scenarios)

Skeleton for RTT scenario:

```cpp
// lat_tcp.cpp
#include <eph/net/kernel/tcp_stream.hpp>
#include <eph/net/kernel/poller.hpp>
#include <eph/codec/raw_stream_codec.hpp>
#include <eph/utils/recorder.hpp>
#include "benchmarks/latency/core/config.hpp"
#include "benchmarks/latency/core/signal.hpp"

namespace en = eph::net::kernel;
namespace ec = eph::codec;
namespace eu = eph::utils;

int main(int argc, char** argv) {
    auto [common, scenario] = bench::load_both("bench.conf", "lat_tcp", argv);

    const auto mock_ip       = common.mock_ip;
    const auto port          = scenario.get_u32("port").value();
    const auto payload_size  = scenario.get_u32("payload_size", 256);
    const auto duration_s    = scenario.get_u32("duration_seconds", 10);
    const auto warmup        = common.warmup_samples;

    auto poller_res = en::Poller::create({});
    auto stream_res = en::TcpStream<ec::RawStreamCodec, false>::create({
        .remote_host = mock_ip,
        .remote_port = static_cast<uint16_t>(port),
    });
    auto stream = std::move(stream_res.value());
    auto poller = std::move(poller_res.value());

    std::vector<uint8_t> payload(payload_size, 0xAB);
    std::vector<uint8_t> latest_rx;

    stream->on_message = [&](const uint8_t* d, uint16_t n) {
        latest_rx.assign(d, d + n);
    };

    poller->add(stream.get()).value();

    // Install SIGINT handler for graceful shutdown
    bench::install_signal_handler();

    // Warmup + measurement loop
    eu::Recorder rec;
    const auto t_deadline = bench::monotonic_raw_ns() + uint64_t(duration_s) * 1'000'000'000ull;
    uint64_t sample_idx = 0;

    while (bench::monotonic_raw_ns() < t_deadline && !bench::shutdown_requested()) {
        auto t0 = bench::monotonic_raw_ns();
        stream->send(std::span{payload}).value();

        latest_rx.clear();
        while (latest_rx.empty()) {
            poller->poll();
        }
        auto t1 = bench::monotonic_raw_ns();

        if (sample_idx >= warmup) {
            rec.push(t1 - t0);  // raw ns input
        }
        ++sample_idx;
    }

    bench::print_report("lat_tcp", scenario, rec);
    return 0;
}
```

Skeleton for one-way scenario (`lat_ex_market`):

```cpp
// lat_ex_market.cpp
int main(int argc, char** argv) {
    auto [common, scenario] = bench::load_both("bench.conf", "lat_ex_market", argv);

    auto stream = en::TcpStream<ec::WsCodec, false>::create({
        .remote_host = common.mock_ip,
        .remote_port = scenario.get_u32("port").value(),
        .ws_path     = scenario.get_string("ws_path", "/ws/bookticker"),
    }).value();

    // ... create poller, attach stream ...

    eu::Recorder rec;
    uint64_t sample_idx = 0;
    const auto warmup = common.warmup_samples;

    stream->on_message = [&](const uint8_t* d, uint16_t n) {
        auto t_recv = bench::monotonic_raw_ns();

        // Hand-written scanner: find "T":<digits> in JSON payload
        auto T_server = bench::scan_json_uint_field(d, n, "T");
        if (!T_server) return;  // malformed, skip

        if (sample_idx >= warmup) {
            rec.push(t_recv - *T_server);
        }
        ++sample_idx;
    };

    // Run until duration expires
    const auto deadline = bench::monotonic_raw_ns() + duration_s * 1'000'000'000ull;
    while (bench::monotonic_raw_ns() < deadline && !bench::shutdown_requested()) {
        poller->poll();
    }

    bench::print_report("lat_ex_market", scenario, rec);
    return 0;
}
```

### JSON scanner helper (hand-written)

`core/json_scan.hpp` (new, ~30 LOC):

```cpp
namespace bench {

/// Find a JSON numeric field value in `payload` by key name.
/// Matches pattern: "<field>":<digits>   (no whitespace tolerance needed for bench mock)
/// Returns std::nullopt if not found or unparseable.
///
/// This is NOT a full JSON parser — it's a purpose-built scanner that
/// works on the known mock payload shape. HFT does not need full RFC 7159
/// in a bench helper.
///
/// Example: scan_json_uint_field(R"({"T":1712345678901,"s":"BTC"})", "T")
///          returns 1712345678901
std::optional<uint64_t> scan_json_uint_field(
    const uint8_t* data, size_t len,
    std::string_view field_name) noexcept;

} // namespace bench
```

Implementation note: use `std::memmem` or manual byte scan + `strtoull`. ~20 LOC.

### Measurement conventions

1. **Clock**: `clock_gettime(CLOCK_MONOTONIC_RAW)` everywhere (client + mock). Not TSC.
2. **Warmup**: `common.warmup_samples` (default 1000) samples discarded before recording
3. **Duration-based stopping**: client runs for `duration_seconds`, then prints and exits
4. **Graceful shutdown**: SIGINT sets flag, current iteration completes, report prints
5. **Recorder input**: raw uint64_t nanoseconds
6. **Report format**:
   ```
   === lat_tcp (kernel | dpdk) ===
   config: port=20000, payload_size=256, duration=10s
   samples: 123456 (warmup 1000 discarded)
   latency_ns:
     min    = 8450
     p50    = 9032
     p99    = 9465
     p99.9  = 10240
     max    = 45120
   ```

---

## 编码规范

| 维度 | 规范 |
|---|---|
| C++ style | 继承 CLAUDE.md + v3.3 conventions (header-only where feasible, std::expected, noexcept hot path) |
| C++ include order | `eph/...` → `benchmarks/latency/core/...` → std |
| Python style | PEP 8, stdlib only, explicit type hints |
| Python shebang | `#!/usr/bin/env python3` |
| Python logging | stderr for progress, stdout for bench data (keeps mock logs separable) |
| INI parser | In-house ~50 LOC (reuse existing config.hpp machinery) |
| Error handling (C++) | std::expected bubble up, fatal on missing required config |
| Error handling (Python) | Exception + stderr message + exit 1 |
| Rate control (Python) | Busy-loop with `clock_gettime` checks, not `time.sleep` (too coarse) |
| WS server (Python) | RFC 6455 strict: verify Sec-WebSocket-Key → SHA1 → base64 → Sec-WebSocket-Accept |
| Commit prefix | `bench(phase-10.N): <title>` |

---

## 实施计划

> **Commit 策略**：每 sub-phase 完成并通过 verification gates 后，subagent 创建 commit，格式 `bench(phase-10.N): <标题>`。每个 sub-phase 是独立回滚点。

**执行方式**：`/repeat` + general-purpose subagent（与 Phase 0-9 一致），每 sub-phase 一个 subagent。

**Verification gates**（每 sub-phase 必须全绿）：

1. **Build gate**: `xmake build <affected_targets>` pass
2. **New tests gate**: 新增 test 全通过
3. **Regression gate**: `xmake run -g tests` 零 FAIL
4. **Coverage gate**: grep -c TEST 新文件 ≥ 承诺数
5. **Style gate**: 对照 feedback_tokio_style.md + CLAUDE.md 自检
6. **Deliverable checklist**: sub-phase checklist 逐项勾选

Sub-phase 10.6 额外加 **Gate 7: Performance verification table** — 对 kernel 6 场景跑 benchmark 输出 p50/p99，对比 floor。

**Rollback protocol**：每 sub-phase 最多 3 fix attempts → `STATUS: BLOCKED`。性能回退特殊：超阈值 > 50ns → 直接 BLOCKED 报告，不 auto-fix（可能是 plan 阈值过严）。

---

### Sub-phase 10.1 — Core 清理 + config 扩展

**交付物**：
- 删除 `core/tsc_protocol.hpp`, `stream_scheduler.hpp`, `ws_framing.hpp`, `ws_handshake.hpp`
- 删除 `tests/unit/bench/test_tsc_protocol.cpp`, `test_tsc_protocol_adversarial.cpp`, `test_stream_scheduler.cpp`, `test_ws_frame.cpp`, `test_ws_handshake.cpp`
- 扩展 `core/config.hpp` 加 `ScenarioConfig` 类 + INI section parser (~100 LOC 新增)
- 扩展 `tests/unit/bench/test_load_bench_conf.cpp` 加 ≥ 10 cases 覆盖 INI section 解析
- 更新 `tests/unit/bench/test_no_dead_headers.cpp` 移除删除的 header 引用
- 新增 `core/json_scan.hpp` + `tests/unit/bench/test_json_scan.cpp` (≥ 8 cases)
- 新增 `core/measurement.hpp`（helper：`monotonic_raw_ns()`, `install_signal_handler()`, `shutdown_requested()`, `print_report()` — 30-50 LOC）
- 更新 `benchmarks/latency/xmake.lua` 如有需要

**Verification checklist**:
- [ ] 5 个旧 header 已删
- [ ] 5 个旧 test 已删
- [ ] ScenarioConfig 能从 INI section 加载
- [ ] get_u32 / get_u64 / get_double / has() API 正确
- [ ] JSON scanner 对 Binance 风格 `{"T":123}` 正确提取
- [ ] monotonic_raw_ns() 在 x86_64 Linux 返回递增 ns
- [ ] test_no_dead_headers 仍 pass
- [ ] test_load_bench_conf +10 cases pass
- [ ] test_json_scan +8 cases pass

**依赖**：无
**预估**：~15-20 min subagent

---

### Sub-phase 10.2 — Python mock 基础设施

**交付物**：
- 创建 `benchmarks/latency/mocks/` 目录
- `mocks/README.md`（说明每个 mock 的目的、如何独立启动、Python 版本要求）
- `mocks/_clock.py`（ctypes wrapper for `clock_gettime(CLOCK_MONOTONIC_RAW)`，~20 LOC）
- `mocks/_rate.py`（busy-loop rate limiter with `monotonic_raw_ns`，~30 LOC）
- `mocks/_ws.py`（RFC 6455 handshake + binary frame encode/decode，~100 LOC）
- `mocks/tcp_echo.py`（stdlib socket TCP echo，~25 LOC）
- `mocks/udp_echo.py`（stdlib socket UDP echo，~25 LOC）
- `mocks/ws_echo.py`（imports _ws，echo loop，~30 LOC）
- `mocks/ex_order_echo.py`（imports _ws，JSON frame passthrough echo，~30 LOC）
- `mocks/ex_market_push.py`（imports _ws + _clock + _rate，WS push with T field in JSON payload，~60 LOC）
- `mocks/ex_md_udp_push.py`（imports _clock + _rate，UDP push with Mold64 format containing per-msg T field，~60 LOC）

**Verification checklist**:
- [ ] 所有 mock 用 `python3 mocks/<mock>.py --help` 打印用法不 crash
- [ ] 所有 mock 能 parse bench.conf 的相关 section
- [ ] `_ws.py` handshake 生成正确的 Sec-WebSocket-Accept（对 baseline test vector）
- [ ] `_rate.py` 在 100 kHz 配置下 10s 内送出 ≥ 950k 包（允许 5% jitter）
- [ ] `_clock.py` 返回 monotonic 递增 ns
- [ ] Python 3.8+ 兼容（ctypes + asyncio 可选）
- [ ] 不依赖 `pip install <anything>`
- [ ] README 包含每个 mock 的独立启动 + 调试命令

**Manual smoke test**（subagent 需跑）：
```bash
# 1. Start each mock in one terminal
python3 mocks/tcp_echo.py --port 20000 &
# 2. Use nc/socat to verify echo
echo "hello" | nc 127.0.0.1 20000
# 3. Kill mock
kill %1
```

**依赖**：10.1 (为 config 解析格式对齐，但 Python mock 自己也实现一个 minimal INI parser)
**预估**：~25-30 min subagent

---

### Sub-phase 10.3 — Rewrite lat_tcp + lat_udp + lat 脚本

**交付物**：
- Rewrite `benchmarks/latency/tcp/lat_tcp.cpp`（v3.3 KernelTcpStream<RawStreamCodec, false> + Recorder + monotonic_raw_ns）
- Rewrite `benchmarks/latency/udp/lat_udp.cpp`（KernelUdpSocket<RawDatagramCodec>）
- xmake 自动 glob 产出 `lat_tcp_dpdk` / `lat_udp_dpdk` sibling（用 DpdkTcpStream / DpdkUdpSocket）
- 重写 `benchmarks/latency/lat` 脚本（dumb dispatcher，~150 LOC bash）
- 扩展 `bench.conf` 加 `[lat_tcp]` 和 `[lat_udp]` sections
- Smoke run: `sudo ./benchmarks/latency/lat tcp` 跑完不崩（可能因为 NIC 权限失败，但 build 必须过）

**Verification checklist**:
- [ ] lat_tcp / lat_tcp_dpdk / lat_udp / lat_udp_dpdk 全 build pass
- [ ] lat_tcp 在 localhost echo 场景下（host-local loopback，bypass NIC 管理）能产生 p50 数字
- [ ] lat_udp 同上
- [ ] lat 脚本 dry-run（--help）不 crash
- [ ] bench.conf 解析成功
- [ ] 数字在合理范围（p50 < 50 μs on loopback）

**依赖**：10.1 (config/measurement) + 10.2 (tcp_echo.py, udp_echo.py)
**预估**：~20-25 min subagent

---

### Sub-phase 10.4 — Rewrite lat_ws + lat_ex_market

**交付物**：
- Rewrite `benchmarks/latency/ws/lat_ws.cpp`（KernelTcpStream<WsCodec, false> + Phase 9.5 transparent WS handshake via `cfg.ws_path`）
- Rewrite `benchmarks/latency/exchange/lat_ex_market.cpp`（one-way measurement via `scan_json_uint_field` for `"T"` 字段）
- xmake glob 产出 `_dpdk` sibling
- 扩展 `bench.conf` 加 `[lat_ws]` 和 `[lat_ex_market]` sections
- Smoke run 确认 build + basic run

**Verification checklist**:
- [ ] 4 个 target build pass
- [ ] lat_ws WS handshake 通过 Phase 9.5 自动集成（StreamConfig.ws_path）
- [ ] lat_ex_market 对 mock 发的 `{"e":"bookTicker",...,"T":<ns>}` 正确提取 T 字段
- [ ] one-way latency 数字合理（应该 < lat_ws RTT / 2）
- [ ] 100 kHz push rate 下，client 不丢包（sample_count 接近 rate × duration）

**依赖**：10.1 + 10.2 (ws_echo.py, ex_market_push.py)
**预估**：~20-25 min subagent

---

### Sub-phase 10.5 — Add lat_ex_order + lat_ex_md_udp

**交付物**：
- NEW `benchmarks/latency/exchange/lat_ex_order.cpp`（N-inflight RTT，128-slot ID table，WS binary frames，hand-written JSON scanner for `"id"` 字段）
- NEW `benchmarks/latency/exchange/lat_ex_md_udp.cpp`（UDP one-way Mold64 with per-msg T field）
- xmake 自动 glob 产出 `_dpdk` siblings
- 扩展 `bench.conf` 加 `[lat_ex_order]` 和 `[lat_ex_md_udp]` sections

**关键实现细节**：

`lat_ex_order` 客户端逻辑：
```cpp
struct IdSlot {
    uint64_t order_id;
    uint64_t t_send;
    bool     in_flight;
};
std::array<IdSlot, 128> table{};

uint64_t next_id = 1;
uint32_t inflight_count = 0;
const uint32_t max_inflight = scenario.get_u32("inflight", 16);

stream->on_message = [&](const uint8_t* d, uint16_t n) {
    auto t_recv = bench::monotonic_raw_ns();
    auto id = bench::scan_json_uint_field(d, n, "id");
    if (!id) return;
    auto slot_idx = *id % 128;
    auto& slot = table[slot_idx];
    if (!slot.in_flight || slot.order_id != *id) return;  // stale or mismatch
    if (sample_idx >= warmup) {
        rec.push(t_recv - slot.t_send);
    }
    ++sample_idx;
    slot.in_flight = false;
    --inflight_count;
};

while (!done) {
    // Send new orders until max_inflight
    while (inflight_count < max_inflight) {
        auto id = next_id++;
        auto slot_idx = id % 128;
        table[slot_idx] = {id, bench::monotonic_raw_ns(), true};
        ++inflight_count;
        // Build JSON: {"e":"NewOrder","id":<id>}
        char buf[64];
        int n = snprintf(buf, sizeof(buf), R"({"e":"NewOrder","id":%lu})", id);
        stream->send(std::span{(const uint8_t*)buf, size_t(n)}).value();
    }
    poller->poll();
}
```

`lat_ex_md_udp` 客户端：
- 用 `KernelUdpSocket<ec::Mold64Codec>`（eph-codec 的 Mold64 codec 自动 parse 包）
- Mock 发的每个 inner msg 自定义 format: `[0:1] type='T'` + `[1:9] server_send_ns` + `[9:17] symbol` + `[17:25] price`
- `on_datagram` 回调中，for each msg in the Mold64 packet:
  - stamp `t_recv = monotonic_raw_ns()`
  - 从 msg bytes 1-8 读 server_send_ns
  - push `t_recv - server_send_ns` to Recorder

**但注意**：eph-codec 的 `Mold64Codec` 只做 packet framing（解 sequence header + split into N msgs），不解 inner msg 内容。Client 需要对每个 inner msg `span<uint8_t>` 手动解 8 bytes server_send_ns。这符合 "no eph-json" 约束。

**Verification checklist**:
- [ ] 4 个 target build pass
- [ ] lat_ex_order 在 inflight=1 下退化为正常 RTT 测量
- [ ] lat_ex_order 在 inflight=16 下 p50 不显著差于 inflight=1（pipelining 不应该恶化 per-order latency）
- [ ] lat_ex_md_udp 对 Mold64 inner msg 数量正确计数（rate × duration × msg_per_packet 左右）
- [ ] 所有场景 on_message 回调 noexcept，不抛异常

**依赖**：10.1 + 10.2 (ex_order_echo.py, ex_md_udp_push.py)
**预估**：~30-35 min subagent（两个新场景，逻辑稍复杂）

---

### Sub-phase 10.6 — Performance verification + Phase 10 closing

**交付物**：
- 执行 kernel 6 场景 benchmark，对比 Phase 9.9 floor + 新采集的 baseline
- 尝试 DPDK 6 场景（best-effort，失败降级 sanity）
- `.artifacts/bench-phase-10-results-20260411.md` 报告：每场景 p50/p99 + delta vs floor
- 更新 `CLAUDE.md`：
  - 更新 "Benchmarks" 段落，列出 6 个 `lat_*` + `_dpdk` sibling
  - 引用 bench.conf 的新 INI section 结构
  - 注明 Python mock 依赖 + rate 上限
- 更新 `benchmarks/latency/README.md`（如存在，否则创建）：
  - 目录结构说明
  - 如何运行单个 scenario
  - bench.conf schema
  - 如何独立启动 mock 调试
- `.artifacts/phase-10-scope-decision.md`（归档本次"不做"的项：payload sweep / 4-leg / eph-json / 1MHz+ / BenchRunner framework）

**Verification checklist (Gate 7 performance table)**:

| 场景 | Floor (Phase 9.9 or newly collected) | Measured p50 | Delta | Status |
|---|---|---|---|---|
| lat_tcp (kernel) | 9032 ns | ? | ? | pass if ≤ +50ns |
| lat_udp (kernel) | (collect now) | ? | — | collected baseline |
| lat_ws (kernel) | 10595 ns | ? | ? | pass if ≤ +50ns |
| lat_ex_market (kernel) | (collect now) | ? | — | collected baseline |
| lat_ex_order (kernel) | (new, no floor) | ? | — | sanity only (latency > 0, < 1ms) |
| lat_ex_md_udp (kernel) | (new, no floor) | ? | — | sanity only |
| lat_*_dpdk | best-effort | ? | — | build pass, run if possible |

- [ ] Gate 7 pass：4 个有 floor 的场景 p50 delta ≤ +50ns
- [ ] 新 scenarios 的 sanity: p50 合理（> 0, < 1ms）
- [ ] DPDK 场景至少 build pass（运行是 nice-to-have）
- [ ] Clean build: `xmake clean && xmake build` pass
- [ ] Full test suite: `xmake run -g tests` 零 FAIL

**依赖**：10.1-10.5 全部完成
**预估**：~25-35 min subagent（有实际 benchmark 运行，可能 60s × 6 场景 = 6-10 分钟 benchmark 时间）

---

## 关键决策记录

### D-1: 去掉 payload sweep 和 4-leg 分解

- **问题**：baseline 每个场景跑 4 档 payload sweep，并且 wire 协议用 24-byte TSC header 做 4-leg 延迟分解。保留还是删除？
- **选项**：
  - A. 完全保留（baseline 等价）
  - B. 保留 sweep 删 4-leg
  - C. 全删，单参数运行，end-to-end 测量
- **决策**：**C**
- **理由**：
  - 4-leg 分解需要 mock 也 stamp 时间戳，强制 mock 懂 wire 协议，限制 mock 实现语言
  - sweep 在代码层引入嵌套 loop + framework，违反"简单直接"原则
  - 用户可以改 bench.conf 重跑来做 sweep，灵活性不丢
  - end-to-end 数字对 HFT 用户是最有意义的
- **验收标准**：
  - 没有 sweep iteration 代码
  - wire 协议不含服务器时间戳字段（mock 不需要 stamp）
  - 报告只有单一配置下的 min/p50/p99/max

### D-2: 测量 helper 复用 Recorder

- **问题**：bench 的直方图/统计用新写 helper 还是复用 eph-utils::Recorder
- **选项**：
  - A. 新写 ~40 LOC `core/stats.hpp`
  - B. 复用 `eph::utils::Recorder`
- **决策**：**B**
- **理由**：
  - 用户明示"统一复用 Recorder，更一致"
  - Recorder 已经测试、已经有 raw ns 输入 API（按用户确认）
  - 省下 40 LOC + 一个测试文件
  - bench 代码与 production monitoring 用同一个统计工具
- **验收标准**：bench scenarios 直接 `#include <eph/utils/recorder.hpp>`，不新增 helper

### D-3: Mock 语言统一 Python stdlib

- **问题**：mock 用什么语言？C / Python / 混合?
- **选项**：
  - A. 每场景最佳：socat (tcp/udp) + Python (ws) + C (push)
  - B. 统一 C
  - C. 统一 Python stdlib
  - D. 统一 C++ 用 eph 库（dogfood）
- **决策**：**C**
- **理由**：
  - 用户明示"统一用一种语言"+"不引入运行复杂度"
  - Python stdlib 在所有 Linux distro 默认可用，零 install
  - 总 LOC ~220（共享 helper + 6 mocks），比 C 的 ~590 少得多
  - Jitter 在 one-way 场景不影响延迟测量（mock stamp 的是实际发送时间，不是预期时间）
  - Python 的 throughput 上限 ~200kHz 够覆盖真实交易所 peak rate
  - 拒绝 D 是因为 dogfood 会掩盖被测代码的 bug
- **验收标准**：
  - `mocks/` 目录下所有文件 `.py` 或 `.md`，无 `.c` `.cpp` `.sh`
  - 无 `pip install <pkg>` 要求
  - `python3 -c "import websockets"` 不需要能成功（不用 websockets 库）
- **限制**：lat_ex_md_udp 默认 push_rate_hz 从 baseline 的 1 MHz 降到 100 kHz，文档说明

### D-4: Client API 完全用 v3.3 Stream/Poller

- **问题**：bench client 用 raw socket fd（baseline 风格）还是 v3.3 Stream/Poller
- **选项**：
  - A. 完全 v3.3
  - B. 完全 raw fd
  - C. 混合（hot path raw fd，setup 用 Stream）
- **决策**：**A**
- **理由**：
  - 用户明示"根据现在的风格"——v3.3 是现在的风格
  - v3.3 Stream 设计为零成本抽象（模板单态 + noexcept + header-only），bench 验证这个承诺
  - 统一风格便于未来维护
  - 若 v3.3 Stream 有隐藏成本，bench 能抓到
- **验收标准**：scenarios 不 include `<sys/socket.h>` / 不调用 `send()` / `recv()` / `sendto()` / `recvfrom()` 裸 syscall

### D-5: 不引入 eph-json

- **问题**：lat_ex_market / lat_ex_order 需要 parse JSON payload，用 eph-json 还是手写 scanner
- **选项**：
  - A. 手写 scanner（`scan_json_uint_field`）
  - B. 用 eph-json Binance codec
  - C. 混合（默认 A，flag 切 B）
- **决策**：**A**
- **理由**：
  - 用户明示"先不引入 eph-json"
  - scan_json_uint_field 是 20 LOC helper
  - mock payload format 受控，不需要完整 JSON parser 的鲁棒性
  - 避免 Phase 10 变成"同时改 eph-json" 的 scope 膨胀
- **验收标准**：
  - `core/json_scan.hpp` 独立 helper，不 include `eph/json/*`
  - scenario 代码不 include `eph/json/*`

### D-6: 测量时钟统一用 `clock_gettime(CLOCK_MONOTONIC_RAW)`

- **问题**：CLAUDE.md 说"TSC 是 canonical"，但 Python mock 不能用 TSC。client 用 TSC 还是 monotonic_raw？
- **选项**：
  - A. Client 用 TSC，one-way 场景时 mock 设法用 TSC（复杂）
  - B. Client 用 monotonic_raw，mock 用 monotonic_raw（简单一致）
  - C. Client 用 TSC 其它场景，one-way 场景特殊用 monotonic_raw
- **决策**：**B**
- **理由**：
  - 用户明示"统一用 clock_gettime(CLOCK_MONOTONIC_RAW)"
  - 在 invariant-TSC x86_64 Linux 上 `clock_gettime(CLOCK_MONOTONIC_RAW)` 通过 vDSO 直接用 TSC，开销 ~20-30ns，与 `rdtsc` 性能等价
  - 提供 ns 粒度，不需要 calibration
  - 跨进程可比较（client 和 Python mock 都能用）
  - CLAUDE.md 的 "TSC canonical" 是 production hot-path 规定；bench 是特殊上下文，本次用户显式 override
- **验收标准**：
  - scenario 代码不 include `eph/utils/tsc.hpp`
  - 不调用 `eph::utils::TSC::now()`
  - 使用 `bench::monotonic_raw_ns()` helper (in core/measurement.hpp)
- **副作用**：Recorder 接受 raw ns（按用户确认已支持），无需转换 wrapper

### D-7: lat_ex_md_udp 默认 rate 100 kHz

- **问题**：baseline lat_ex_md_udp 在 1 MHz 下跑。Python mock 跑不到 1 MHz（上限 ~200 kHz）。降 rate 还是换 mock 语言？
- **选项**：
  - A. 保持 1 MHz，mock 用 C
  - B. 降到 100 kHz，Python mock
  - C. 保持 1 MHz，Python mock（接受丢包/jitter）
- **决策**：**B**
- **理由**：
  - 用户选了 Python mock 统一栈
  - 100 kHz 对 HFT 用户价值足够（CME 单 feed peak ~100-500 kHz，Binance ~50-100 kHz）
  - 1 MHz 是 stress test，不是典型场景
  - 用户想测 1 MHz 可以自己替换 mock，bench.conf 改 rate 即可
- **验收标准**：
  - bench.conf 默认 `[lat_ex_md_udp] push_rate_hz = 100000`
  - `mocks/ex_md_udp_push.py` 注释说明上限 ~200 kHz
  - `mocks/README.md` 明示"更高 rate 需替换 mock 为 C"

---

## 风险与限制

| 风险 | 缓解 |
|---|---|
| Python mock jitter 在 100 kHz 下污染测量 | one-way 测量不受 jitter 影响（mock stamp 实际发送时间）；RTT 场景 mock 是纯 echo，jitter 不介入 latency 计算 |
| DPDK 场景需要 vfio-pci 硬件 | 10.6 best-effort，失败降级 sanity；不强制 |
| Recorder 不支持 raw ns（若 user 判断错误） | 10.1 先验证 Recorder API；若真的不支持，加 10-20 LOC wrapper 或降级新 helper |
| 删除的 5 个 header 可能被 stale import 引用 | test_no_dead_headers.cpp 作为 compile-time gate |
| Python 版本兼容（< 3.8 可能缺 feature） | README 明示要求 Python 3.8+；大多数 distro 自 2020 年起默认满足 |
| `lat` 脚本 NIC 状态管理在非预期 host 上挂起 | 加 `--dry-run` flag for debug；保留现有脚本的 idempotent 逻辑 |

---

## 完成信号

Phase 10 完成的标志：
1. 6 个 sub-phase 全部 commit，最后 review APPROVE
2. 6 个 kernel scenario binary 产生可用输出
3. kernel 性能对比：有 floor 的 4 场景 p50 delta ≤ +50ns
4. 6 个 `_dpdk` sibling 至少 build pass
5. `lat` 脚本能 dispatch 所有 6 scenarios
6. bench.conf 示例有全部 6 section
7. Mock 6 个 `.py` 文件存在，`python3 --help` 每个都能跑
8. CLAUDE.md 和 benchmarks/latency/README.md 更新完成
9. 完整 test suite 零 FAIL
10. `.artifacts/phase-10-scope-decision.md` 归档

此后可以认为 `benchmarks/latency/` 与 baseline 语义等价，refactor/transport-api 分支 benchmark 层面 ready for merge。

---

## 状态：已确认

等待用户确认或直接启动 `/repeat` 执行 Phase 10.1-10.6。
