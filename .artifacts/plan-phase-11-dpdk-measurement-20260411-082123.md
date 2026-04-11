# Plan: Phase 11 — DPDK real measurement loops for benchmarks/latency

> Wire the six `lat_*` scenario binaries' `#if EPH_USE_DPDK` branches to drive
> real DpdkTcpStream / DpdkUdpSocket measurement loops over NIC_B, replacing
> the Phase 10.6 "deferred" banner. Single sub-phase, subagent-executed.

创建时间：2026-04-11
状态：已确认

---

## 定位与边界

**目标**：将 kernel vs DPDK 的 client-side code path 对比从"只有 kernel 是实测，DPDK 只 compile-check"升级为"两个 backend 都实测同一物理 NIC 路径"，让 `sudo ./lat tcp --dpdk` 等 6 个命令产出真实 DPDK 延迟数据。

**用户**：eph 项目维护者 / HFT transport 设计评估人 — 需要验证 v3.3 DpdkTcpStream / DpdkUdpSocket 在公平物理 NIC 路径下的实际延迟相对 kernel 的收益。

**In scope**:
- `benchmarks/latency/{tcp,udp,ws,exchange}/lat_*.cpp` 6 个文件的 `#if defined(EPH_USE_DPDK)` 分支
- 新增 `benchmarks/latency/core/dpdk_env.hpp` 共享 helper（bench.conf → DpdkBenchEnv::create_full 的一次性桥接）
- `benchmarks/latency/core/measurement.hpp` `print_report` 增强：新增 avg / stddev / p90 / throughput 字段（利用 `Recorder::Stats` 已有数据，kernel+dpdk 同时受益）
- `lat_*_dpdk` target 在 `xmake.lua` 中已存在；本 phase 不新增 target，只填充 `main()`
- DPDK 特定 config echo（eal_cores / pci / port_id / src_mac / gw_mac）和 backend marker

**Out of scope**:
- 不引入新的 measurement 拓扑（仍沿用 Phase 10 的 client-drives-mock 模式，mock 永远 kernel，per CLAUDE fairness doctrine）
- 不修改 bench.conf schema（复用 mock_ip / client_ip / gateway_ip / client_nic / eal_cores / NIC_B_PCI）
- 不引入新的 framer / codec；TCP 系用 `RawStreamCodec`（WS 系用 `WsCodec`），UDP 系用 `RawDatagramCodec`
- 不改 lat 包装脚本（它已经正确处理 NIC_B 的 host↔bench_ns↔vfio-pci 状态机）
- 不引入 payload sweep 或 inflight 矩阵（仍单值 payload + 单值 duration per bench.conf）
- 不改 mock 侧（mock 仍永远 kernel，Python）
- 不做 DPDK vs kernel 跨场景的 aggregate 报告生成（用户可手动对比 stdout）

**Non-goals**:
- 不追求 sub-μs 极致优化（this phase 只是把 "Real DPDK measurement loop is deferred" 的占位改成真实测量循环，不调 EAL 参数 / 不做 PMD tuning）
- 不追求 lat_*_dpdk 二进制的可重入性（一次运行一个 scenario 就够了，EAL 单例语义，main 退出即释放）

---

## 技术选型

| 类别 | 选择 | 理由 |
|---|---|---|
| DPDK bootstrap | `eph::dpdk::test::DpdkBenchEnv::create_full(argc, argv, server_ip, local_ip, gateway_ip, port_id)` | 已在 `eph-net-dpdk/include/eph/dpdk/test/dpdk_env.hpp` 提供，集成 EAL init + Platform + ARP resolve 6 步流程；test 和 bench 共用，不复制代码 |
| EAL argv 来源 | 从 bench.conf 读 `eal_cores` / `EAL_CORES` + `NIC_B_PCI`，bench 二进制内部合成 EAL argv 后传给 create_full | 用户 CLI 保持 `sudo lat tcp --dpdk` 不变；EAL 参数不污染 bench 的 `--config` flag 空间 |
| TCP stream 类型 | `eph::net::dpdk::DpdkTcpStream<eph::core::RawStreamCodec, /*EnableTls=*/false>` | lat_tcp kernel 侧正在用 `RawStreamCodec`；DPDK 侧保持 codec 一致保证对比公平；TLS=false 因为 bench mock 不跑 TLS |
| WS stream 类型 | `eph::net::dpdk::DpdkTcpStream<eph::core::WsCodec, /*EnableTls=*/false>` | 和 kernel lat_ws 一致 |
| UDP socket 类型 | `eph::net::dpdk::DpdkUdpSocket<eph::core::RawDatagramCodec>` | 和 kernel lat_udp / lat_ex_md_udp 一致 |
| Poller 类型 | `eph::net::dpdk::DpdkPoller<>` (type-erased default) | 和 kernel 侧 KernelPoller 对称，add/poll/remove API 一致，scenario 代码 template 一份 |
| ARP resolve | `create_full` 内部调 `eph::dpdk::arp::resolve(port_id, 0, pool, src_mac, src_ip, gw_ip, 3s)` | 已有实现，不需要额外工作 |
| src_port 分配 | 每次 `main()` 启动随机选 `[49152, 65535]` 内的端口 | DPDK TcpSession 不走内核，端口不进 TIME_WAIT 表，但避免 mock 侧 kernel TIME_WAIT 污染（mock bind 固定 server 端口，accept 的 tuple 缓存短时间内重连需要不同 src_port） |
| 度量 clock | 继续用 `bench::monotonic_raw_ns()` (Phase 10 D-6) | 和 kernel 侧一致，Recorder::record_ns 无需改动 |
| stats 增强 | 只扩 `print_report`，不扩 `Recorder` | Recorder::Stats 已经有 avg_ns / stddev_ns / p90_ns，我们只是把它们打印出来；不触碰 eph-utils 层 |

---

## 架构设计

### 模块划分

| 文件 | 职责 | 依赖 |
|---|---|---|
| `benchmarks/latency/core/dpdk_env.hpp` *(新)* | 读 bench.conf 的 DPDK 相关 key（eal_cores/NIC_B_PCI/mock_ip/client_ip/gateway_ip），合成 EAL argv，调 `DpdkBenchEnv::create_full`；导出 `load_dpdk_env(ScenarioConfig& globals, uint16_t dpdk_port_id)` | `eph/dpdk/test/dpdk_env.hpp`、`core/scenario_config.hpp` |
| `benchmarks/latency/core/measurement.hpp` *(改)* | `print_report` 增加 avg / stddev / p90 / throughput 字段；backend 字段照旧；接口签名不变 | `eph::utils::Recorder` |
| `benchmarks/latency/tcp/lat_tcp.cpp` *(改)* | `#if EPH_USE_DPDK` 分支内：load config → load_dpdk_env → make_tcp_config → DpdkTcpStream::create → DpdkPoller::add → 同 kernel 分支的 measurement loop → print_report | `core/dpdk_env.hpp`、`eph/net/dpdk/tcp_stream.hpp`、`eph/net/dpdk/poller.hpp` |
| `benchmarks/latency/udp/lat_udp.cpp` *(改)* | 同上，UDP 版本：make_udp_sender + DpdkUdpSocket::create + RawDatagramCodec | `eph/net/dpdk/udp_socket.hpp` |
| `benchmarks/latency/ws/lat_ws.cpp` *(改)* | 同 TCP，使用 WsCodec | WS handshake 复用 kernel 侧相同路径（WsCodec 自己做 upgrade） |
| `benchmarks/latency/exchange/lat_ex_market.cpp` *(改)* | 同 WS；RX-only push 测量（1-way latency via mock 的 T_push 时戳字段） | 同 lat_ws |
| `benchmarks/latency/exchange/lat_ex_order.cpp` *(改)* | 同 WS；128-slot inflight 序号表；pipelining loop | 同 lat_ws |
| `benchmarks/latency/exchange/lat_ex_md_udp.cpp` *(改)* | 同 UDP；Mold64 outer + 8-byte LE T field 1-way 测量 | 同 lat_udp |

### 核心抽象

**`bench::load_dpdk_env`** — 唯一新增抽象，把"从 ScenarioConfig 读取 DPDK 相关 key、合成 EAL argv、调 create_full"封装成一个函数，6 个 scenario 共用一份代码：

```cpp
// benchmarks/latency/core/dpdk_env.hpp
#pragma once
#ifdef EPH_USE_DPDK

#include <expected>
#include <string>
#include <vector>

#include "eph/dpdk/test/dpdk_env.hpp"
#include "benchmarks/latency/core/scenario_config.hpp"

namespace bench {

/// Read DPDK bootstrap keys from the global section of bench.conf and
/// construct a fully initialized DpdkBenchEnv (EAL init + Platform +
/// ARP resolve).
///
/// Required keys (checked in this order, lowercase first, uppercase fallback):
///   - mock_ip      / SERVER_IP     — destination (kernel mock, ens34)
///   - client_ip    / LOCAL_IP      — local IP on ens35
///   - gateway_ip   / GATEWAY_IP    — default GW for ARP resolve
///   - eal_cores    / EAL_CORES     — comma-separated lcore list (e.g. "0,1")
///   - dpdk_pci     / NIC_B_PCI     — NIC_B PCI BDF (e.g. "0000:28:00.0")
///
/// Synthesizes EAL argv like:
///   [ "lat_bench", "-l", "0,1", "-a", "0000:28:00.0", "--proc-type=auto", "--" ]
/// and passes it to `DpdkBenchEnv::create_full` together with IPs and
/// `dpdk_port_id=0` (there is always exactly one PCI device allow-listed
/// so port 0 is unambiguous).
///
/// Returns the constructed env on success or an error string describing
/// which step failed (missing key, EAL init, Platform create, ARP).
[[nodiscard]] std::expected<eph::dpdk::test::DpdkBenchEnv, std::string>
load_dpdk_env(const ScenarioConfig& globals, uint16_t dpdk_port_id = 0) noexcept;

} // namespace bench

#endif // EPH_USE_DPDK
```

实现细节：
- 函数内部申请 `std::vector<std::string>` 存 argv 字符串，再用 `std::vector<char*>` 暴露 `argv` 给 `create_full`（字符串容器保活到 `create_full` 返回即可，因为 EAL init 会自己拷贝）
- 读 `eal_cores` 时如果既没有 lowercase 也没有 uppercase，默认 `"0,1"` 并打 WARN；读 `dpdk_pci` 则必须存在，缺失返回 error
- `mock_ip` / `client_ip` / `gateway_ip` 必须存在，缺失返回 error（和 kernel 侧 `lat_*` 读 mock_ip 的默认 "127.0.0.1" 不一样 — DPDK 下 loopback 没有意义，必须显式）

**DpdkBenchEnv 已有 `make_tcp_config(local_port, remote_port)` 和 `make_udp_sender(local_port, remote_port)`** — 直接使用，不新增 helper。

**DpdkTcpStream::StreamConfig** — 把 `DpdkBenchEnv::make_tcp_config(...)` 的返回值赋给 `cfg.legacy`，其它字段保持默认（reconnect / tls / ws_path 都是 StreamConfig 的现有字段）：

```cpp
auto env = *bench::load_dpdk_env(globals, /*port_id=*/0);
ed::StreamConfig cfg{};
cfg.legacy = env.make_tcp_config(bench::random_src_port(), port);
cfg.reasm_capacity  = std::max<std::size_t>(64 * 1024, payload_size * 4);
cfg.connect_timeout = std::chrono::milliseconds{3000};
// WS only: cfg.ws_path / cfg.ws_host from bench.conf [lat_ws] section
auto stream_r = DpdkStream::create(cfg);
```

### 数据流（lat_tcp_dpdk 示例）

```
main(argc, argv)
 ├─ parse_config_path(argc, argv)                 // 和 kernel 侧相同
 ├─ ScenarioConfig::load_globals(conf_path)
 ├─ ScenarioConfig::load(conf_path, "lat_tcp")
 ├─ bench::load_dpdk_env(globals, 0)              // ⬅️ DPDK 专属
 │    ├─ read mock_ip / client_ip / gateway_ip / eal_cores / dpdk_pci
 │    ├─ synthesize eal_argv = ["lat_tcp_dpdk", "-l", "0,1",
 │    │                         "-a", "0000:28:00.0", "--proc-type=auto", "--"]
 │    └─ DpdkBenchEnv::create_full(eal_argc, eal_argv, mock_ip, client_ip, gw_ip, 0)
 │        ├─ EalGuard::init(eal_argc, eal_argv)
 │        ├─ Platform::create({port_id=0})
 │        ├─ parse 3 IPs → host byte order
 │        ├─ rte_eth_macaddr_get → src_mac
 │        └─ arp::resolve(port 0, 0, pool, src_mac, src_ip, gw_ip, 3s) → gw_mac
 ├─ install_signal_handler()
 ├─ DpdkPoller<>::create({port_id=0, rx_queue_id=0, rx_cpu=cpu_client})
 ├─ DpdkStream::create(cfg {.legacy = env.make_tcp_config(random_src_port, port)})
 │    └─ internally: TcpSession::connect(3s) — 3-way handshake on real NIC
 ├─ poller->add(stream.get())
 ├─ for t0..t_deadline:
 │    stream->send(payload)         // direct DPDK mbuf TX
 │    while rx_bytes < payload_size:
 │      poller->poll()               // rte_eth_rx_burst drains mbufs
 │    rec.record_ns(monotonic_raw_ns() - t0)
 ├─ bench::print_report("lat_tcp", "dpdk", rec, warmup_samples)
 │    + print_dpdk_config_echo(env)   // NEW: eal_cores / pci / port / mac
 └─ stream->close_gracefully() / poller->remove / stream.reset / poller.reset
```

**UDP 路径** 结构相同，差异仅在：
- 用 `DpdkUdpSocket<RawDatagramCodec>` + `env.make_udp_sender(local_port, remote_port)` 获得底层 sender
- 无 3-way handshake，无 connect_timeout
- Recv 路径用 `poller->poll()` 回调 `on_message`，但匹配是 payload_size（和 kernel lat_udp 对称）

**lat_ex_md_udp** 和 **lat_ex_market** 是 push-only 1-way 场景：
- 没有 client-side send
- 测量 `now_ns - T_field` 作为 1-way latency
- DPDK 分支和 kernel 分支的 measurement loop 完全一致，只是 socket 类型换成 DpdkUdpSocket / DpdkTcpStream+WsCodec

**lat_ex_order** 是 pipelined request/response：
- 128-slot id table 追踪 inflight order
- kernel 和 DPDK 的 loop 结构完全相同，只是 stream 类型不同

---

## 接口设计

### 公共 API

**新增**：`bench::load_dpdk_env(globals, port_id)` — 见上方"核心抽象"。

**增强**：`bench::print_report(scenario_name, backend, rec, warmup_discarded)` — **接口签名不变**，只是 body 多打几个字段：

```
=== lat_tcp (dpdk) ===
samples: 1234567 (warmup 1000 discarded)
latency_ns:
  min    = 12345
  p50    = 15678                   ← 已有
  p90    = 18000                   ← 新
  p99    = 22000                   ← 已有
  p99.9  = 30000                   ← 已有
  max    = 55000                   ← 已有
  avg    = 16234                   ← 新
  stddev = 1850                    ← 新
throughput: 123456 samples/s       ← 新（count / wall_time_s）
```

注意 throughput 需要 wall_time_s，所以 `print_report` 的签名其实需要加一个可选参数 `double wall_time_s = 0.0`，为 0 时不打印 throughput。但为了不破坏现有调用点，我们用**函数重载**或**在 Recorder 上读**：

**决定**：给 `print_report` 加一个**尾部带默认值**的新参数 `uint64_t wall_time_ns = 0`，保持向后兼容：

```cpp
inline void print_report(std::string_view scenario_name,
                         std::string_view backend,
                         eph::utils::Recorder& rec,
                         uint64_t warmup_discarded = 0,
                         uint64_t wall_time_ns = 0) noexcept;
```

所有 6 个 kernel scenario 和 6 个 DPDK scenario 都传 `wall_time_ns = monotonic_raw_ns() - t_measure_start`（t_measure_start 是 warmup 结束那一刻的时戳），不为 0 时 `print_report` 就多打一行 throughput。

**新增**：`bench::print_dpdk_config_echo(env)` — 打印 DPDK 特定的 config 摘要，format:

```cpp
inline void print_dpdk_config_echo(const eph::dpdk::test::DpdkBenchEnv& env) noexcept {
    std::printf("dpdk_config: port_id=%u "
                "src_mac=%02x:%02x:%02x:%02x:%02x:%02x "
                "gw_mac=%02x:%02x:%02x:%02x:%02x:%02x "
                "src_ip=0x%08x dst_ip=0x%08x gw_ip=0x%08x\n",
                env.port_id,
                env.src_mac.addr_bytes[0], ..., env.src_mac.addr_bytes[5],
                env.gw_mac.addr_bytes[0], ..., env.gw_mac.addr_bytes[5],
                env.src_ip, env.dst_ip, env.gw_ip);
}
```

放在 `core/dpdk_env.hpp` 里，scenario 在 `print_report` 之前调用一次。

### 错误体系

- `load_dpdk_env` 返回 `std::expected<DpdkBenchEnv, std::string>`，错误消息带前缀 `load_dpdk_env: <step>: <detail>`，scenario 打 stderr 后 `return 1`
- `DpdkTcpStream::create` / `DpdkUdpSocket::create` 仍返回 `core::ErrorInfo`，scenario 打 `r.error().detail` 后 `return 2`
- `DpdkPoller::add` 同上 → `return 3`
- 单次 recv 超时（`t - t0 > 5s`）→ `timed_out = true` break，最后 `return 4`
- 所有错误路径必须 `poller->remove(stream.get()); stream.reset(); poller.reset();` 保证 EAL 正确释放（`DpdkBenchEnv` 析构负责 EAL teardown）

---

## 编码规范

| 维度 | 规范 |
|---|---|
| 命名 | `load_dpdk_env` / `print_dpdk_config_echo` — snake_case，符合现有 `bench::` 命名风格 |
| 错误处理 | `std::expected<T, std::string>` for `load_dpdk_env`（和 `DpdkBenchEnv` 本身一致）；`std::expected<T, core::ErrorInfo>` for stream/poller |
| 日志 | INFO 级打配置 echo 和 ARP resolved MAC（`DpdkBenchEnv::create_full` 已经做了）；DEBUG 级打每个连接 tuple；TRACE 级可打每 N 万样本一次的进度（仿 kernel 侧） |
| 注释 | 在 scenario 的 `#if EPH_USE_DPDK` 分支顶部加 3-5 行注释说明"对比 kernel 分支：只是 backend 替换，measurement loop 字字相同"；`load_dpdk_env` 实现处注释 EAL argv 合成的理由 |
| includes | 保持 `eph/` 绝对路径前缀；不使用相对路径 |
| EAL 参数 | 硬编码 `--proc-type=auto`，不暴露给 bench.conf（未来如有需要可扩展，但 YAGNI） |
| src_port 随机化 | `static thread_local std::mt19937 rng{std::random_device{}()};` + `std::uniform_int_distribution<uint16_t>{49152, 65535}`，封装在 `bench::random_src_port()` 里 |

---

## 实施计划

> **Commit 策略**：单 commit，消息格式 `bench(phase-11.0): wire real DPDK measurement loops across 6 lat_* scenarios`。commit body 列出 6 个 scenario 文件 + `core/dpdk_env.hpp`（新）+ `core/measurement.hpp`（增强）+ 每个 gate 的结果摘要。不拆 sub-commits。

### 阶段 11.0: DPDK real measurement loops — all 6 scenarios

**交付物**:

1. **新文件** `benchmarks/latency/core/dpdk_env.hpp`（~80 LOC）
   - `bench::load_dpdk_env(globals, port_id=0)` 实现
   - `bench::print_dpdk_config_echo(env)` 实现
   - `bench::random_src_port()` 实现
   - 全文 `#ifdef EPH_USE_DPDK` 包围

2. **修改** `benchmarks/latency/core/measurement.hpp`
   - `print_report` 增加 `wall_time_ns = 0` 参数
   - 如果 `stats_opt` 非空则额外打 p90 / avg / stddev
   - 如果 `wall_time_ns > 0` 且 `s.count > 0` 则额外打 throughput（samples/s）
   - **同时**更新所有 6 个 kernel scenario 的 `print_report` 调用，传入 `wall_time_ns` — 确保 kernel 输出也多出 p90/avg/stddev/throughput（巩固 kernel vs dpdk 对比格式对齐）

3. **修改** 6 个 lat scenario 文件，填充 `#if EPH_USE_DPDK` 分支：
   - `tcp/lat_tcp.cpp`: DpdkTcpStream<RawStreamCodec, false>
   - `udp/lat_udp.cpp`: DpdkUdpSocket<RawDatagramCodec>
   - `ws/lat_ws.cpp`: DpdkTcpStream<WsCodec, false> + ws_path/ws_host from [lat_ws]
   - `exchange/lat_ex_market.cpp`: DpdkTcpStream<WsCodec, false> + 1-way push measurement with T field
   - `exchange/lat_ex_order.cpp`: DpdkTcpStream<WsCodec, false> + 128-slot inflight pipelining
   - `exchange/lat_ex_md_udp.cpp`: DpdkUdpSocket<RawDatagramCodec> + Mold64 T-field 1-way

4. **每个 scenario 的 DPDK 分支结构统一为**:
   ```
   parse_config_path → load_globals → load_scenario
   → bench::load_dpdk_env → DpdkPoller::create
   → DpdkStream::create (via env.make_tcp_config or env.make_udp_sender)
   → poller->add
   → install_signal_handler
   → print config echo + print_dpdk_config_echo
   → measurement loop (字面拷贝 kernel 分支的 loop，只改 stream 类型)
   → t_measure_start 记录 warmup 结束时戳
   → print_report(name, "dpdk", rec, warmup_samples, now_ns - t_measure_start)
   → graceful teardown
   ```

**验收标准**:

1. **Build gate**: `xmake f --use_dpdk=y && xmake build -g benchmarks` 成功产出 6 个 `lat_*_dpdk` 二进制；kernel 版 6 个二进制继续构建成功
   - 如果 `--use_dpdk=y` flag 不存在（DPDK 是 default-on）则省略该 flag
   - 检查命令：`ls build/*/*/release/lat_{tcp,udp,ws,ex_market,ex_order,ex_md_udp}_dpdk`

2. **Kernel regression gate**: 所有 6 个 kernel scenario 在 bench_ns 物理 NIC 路径下运行成功，p50 与 Phase 10.6 最后一次 kernel 基线对比无显著偏差（±10% 内）：
   - `sudo ./benchmarks/latency/lat tcp`
   - `sudo ./benchmarks/latency/lat udp`
   - `sudo ./benchmarks/latency/lat ws`
   - `sudo ./benchmarks/latency/lat ex_market`
   - `sudo ./benchmarks/latency/lat ex_order`
   - `sudo ./benchmarks/latency/lat ex_md_udp`

3. **DPDK live gate**: 所有 6 个 DPDK scenario 在 vfio-pci 物理 NIC 路径下运行成功，产出非空 stats 且不 time out：
   - `sudo ./benchmarks/latency/lat tcp --dpdk`
   - `sudo ./benchmarks/latency/lat udp --dpdk`
   - `sudo ./benchmarks/latency/lat ws --dpdk`
   - `sudo ./benchmarks/latency/lat ex_market --dpdk`
   - `sudo ./benchmarks/latency/lat ex_order --dpdk`
   - `sudo ./benchmarks/latency/lat ex_md_udp --dpdk`
   - 每个 scenario 的 stdout 必须包含 `samples: <N>` 且 `N > warmup_samples`，且 `max_ns < 5 * 10^9`

4. **Fairness gate**: 对于 `lat_tcp` / `lat_udp` / `lat_ws`，DPDK 的 p50 **必须低于** kernel 的 p50（AWS VPC fabric 路径下 DPDK 去掉 kernel 栈应至少快 3-10 μs）。如果 DPDK p50 ≥ kernel p50 → STATUS: BLOCKED，很可能是 wiring bug（例如 stream->send 被 block 在 syscall，或 measurement clock 没对齐，或 TcpSession 没走 zero-copy 路径）。该 gate 不检查 exchange scenario（push-rate 限流掩盖 backend 差异）。

5. **Report completeness gate**: 所有 12 次运行的 stdout 必须包含这些字段（grep 检查）：
   - `samples:` `min` `p50` `p90` `p99` `p99.9` `max` `avg` `stddev` `throughput:`
   - kernel 和 dpdk 两种 backend 各 6 次
   - 共 12×10 = 120 次 grep 全绿

6. **Config echo gate**: DPDK 运行的 stdout 必须包含：
   - `dpdk_config: port_id=` 行
   - `src_mac=` / `gw_mac=` / `src_ip=` / `dst_ip=` / `gw_ip=` 字段齐全
   - 6 次 DPDK 运行全绿

7. **No-regression on unit tests**:
   - `xmake build -g tests && xmake run -g tests` 全绿
   - 特别是 `tests/unit/bench/*` 下的 scenario_config / measurement 测试不受 `print_report` 签名扩展影响（因为新参数有默认值）
   - `tests/integration/test_dpdk_e2e` 全绿（保证 DpdkBenchEnv::create_full 在 bench 和 test 共用后 test 侧仍能跑）

8. **Deliverable checklist**:
   - [ ] `core/dpdk_env.hpp` 新文件存在
   - [ ] `core/measurement.hpp` `print_report` 签名带 `wall_time_ns` 参数
   - [ ] 6 个 scenario 的 `#if EPH_USE_DPDK` 分支非空，不再打印 "deferred" banner
   - [ ] 6 个 kernel 分支的 `print_report` 调用传入 wall_time_ns
   - [ ] 单 commit，消息格式正确
   - [ ] `/repeat` 派发的 subagent 退出时提交报告

**推荐 skill**: `/design auto` (单 subagent 完成整个 11.0)

**预估**: 1 个 subagent 会话（~400 行新增 + ~200 行修改，≈20 分钟 wall clock）

---

## 关键决策记录

### D-1: 把 `DpdkBenchEnv` 放哪里？

- **问题**：`load_dpdk_env` 需要的 `DpdkBenchEnv` 已经在 `eph-net-dpdk/include/eph/dpdk/test/dpdk_env.hpp`。是否需要把它挪到 `benchmarks/latency/core/`？
- **选项**：
  - A. 继续从 `eph/dpdk/test/` 引用（test 和 bench 共享同一份）
  - B. 拷贝一份到 `benchmarks/latency/core/dpdk_env.hpp`
  - C. 挪到 `eph-net-dpdk/include/eph/net/dpdk/bench_env.hpp`（重命名 namespace）
- **决策**：A
- **理由**：`eph::dpdk::test::DpdkBenchEnv` 名字虽然带 `test::` namespace，但其职责"EAL+Platform+ARP 一次性 bring-up"对 test 和 bench 都适用，没有实际 test-only 的代码。重复代码比 cosmetic namespace 更糟。benchmarks/latency/core/dpdk_env.hpp 只做"从 ScenarioConfig 读配置 → 合成 EAL argv → 调 create_full"的 bench 专属薄壳。
- **验收**：`benchmarks/latency/core/dpdk_env.hpp` 中 `#include "eph/dpdk/test/dpdk_env.hpp"` 存在且编译通过。

### D-2: EAL 参数从哪里来？

- **问题**：DPDK EAL 需要 `-l <cores>` 和 `-a <pci>` 等参数。用户 CLI 是 `sudo lat tcp --dpdk`，怎么把 EAL 参数塞进去？
- **选项**：
  - A. 用户在 `lat` wrapper 脚本里 export 环境变量 `EAL_ARGS="-l 0,1 -a 0000:28:00.0"`，bench 二进制读 env
  - B. bench 二进制从 `bench.conf` 读 `eal_cores` / `dpdk_pci`，内部合成 argv
  - C. 用户直接传 `lat tcp --dpdk -- -l 0,1 -a 0000:28:00.0`，bench 二进制 argc 里已经有了
- **决策**：B
- **理由**：用户的 CLI 体验应该保持"一个命令跑完一个场景"，不让他记 EAL 参数。bench.conf 本来就是 latency bench 的唯一真源（NIC / IP / CPU 都在那里），EAL 参数放一起最自然。A 方案的 env 路径不易 reproducible、C 方案太难记。
- **验收**：`bench.conf` 里 `eal_cores=0,1` 和 `NIC_B_PCI=0000:28:00.0` 存在（当前已存在），bench 二进制 `main` 无须任何额外 CLI flag 即可跑起 DPDK。

### D-3: src_port 每次都随机吗？

- **问题**：DPDK TcpSession 需要 `(src_ip, src_port, dst_ip, dst_port)` 四元组。src_port 固定会不会碰到 mock 侧 kernel 的 TIME_WAIT？
- **选项**：
  - A. 固定 src_port = 50000（每次都碰 TIME_WAIT）
  - B. 每次 main() 随机选 ephemeral 范围
  - C. 让用户在 bench.conf 里配
- **决策**：B
- **理由**：mock 是 kernel，accept 过的 4-tuple 会进 TIME_WAIT 60s。用户可能 5 秒内连跑两次 `lat tcp --dpdk` 做对比，固定端口必翻车。随机化零成本、零配置。
- **验收**：`bench::random_src_port()` 存在，每次 main() 进入时调用一次，赋给 `make_tcp_config` 的 local_port 参数。

### D-4: `print_report` 加字段还是加函数？

- **问题**：kernel 和 DPDK 两种 backend 要不要打不同的字段？DPDK 要额外打 src_mac / gw_mac / port_id。
- **选项**：
  - A. 扩 `print_report` 成 variadic，backend 传回调
  - B. `print_report` 只打 stats；额外的 DPDK config echo 用独立的 `print_dpdk_config_echo(env)` 函数
  - C. 不打 config echo，只打 stats
- **决策**：B
- **理由**：scenario 主循环跑完后顺序调 `print_dpdk_config_echo(env)` 和 `print_report(...)` 两个函数，干净简单。把所有 stats 字段（min/p50/p90/p99/p999/max/avg/stddev/throughput）打在同一行/段对两种 backend 都有好处，不需要按 backend 分流。DPDK 特有的 MAC/port 信息独立一行不污染 stats 表格。
- **验收**：`print_report` 的 6 个 kernel 调用无需改变结构（只是新加 `wall_time_ns` 尾参）；DPDK 分支在 `print_report` 之前多一次 `print_dpdk_config_echo(env)` 调用。

### D-5: Phase 11.0 是不是也拿出来做 `core/dpdk_env.hpp` 的单测？

- **问题**：新加 `bench::load_dpdk_env` 是纯 helper，但它依赖 EAL init（不能在无 DPDK 的 CI host 跑）。是否给它写单测？
- **选项**：
  - A. 写单测，跳过条件 `if (!getenv("DPDK_AVAILABLE")) GTEST_SKIP`
  - B. 不写单测，靠 DPDK live gate 覆盖
  - C. 只测 argv 合成逻辑（不调 create_full）
- **决策**：C
- **理由**：B 的问题是 load_dpdk_env 里的 argv 合成逻辑（把 `"0,1"` 拆成 `-l 0,1`、把 PCI 变成 `-a 0000:28:00.0`、加 `--proc-type=auto`）是纯字符串操作，逃不过单测。把这部分提到一个独立的 `bench::synthesize_eal_argv(cores, pci) -> std::vector<std::string>` 函数，然后在 `tests/unit/bench/test_dpdk_env_argv.cpp` 里单测它。load_dpdk_env 的剩余部分（读 config + 调 create_full）是 thin glue，不需要单测。
- **验收**：`tests/unit/bench/test_dpdk_env_argv.cpp` 新增，至少 3 个 TEST case：单核、多核、带 PCI 无 PCI 各一。

### D-6: Fairness gate 到底多严？

- **问题**：DPDK 理论上应该比 kernel 快，但具体快多少？gate 设多严格才不会误判？
- **选项**：
  - A. DPDK p50 ≤ kernel p50（任何正数收益即通过）
  - B. DPDK p50 ≤ kernel p50 × 0.9（至少 10% 收益）
  - C. DPDK p50 ≤ kernel p50 - 3000 ns（至少 3 μs 绝对收益）
- **决策**：A，但带 SOFT WARN
- **理由**：AWS VPC fabric 的 PCIe 往返时间是主要开销，DPDK 去掉 kernel 栈的 syscall + softirq + scheduler 节省 3-10 μs 理论可观，但 VPC fabric 有 ~15-25 μs 抖动，实测可能被抖动淹没。如果连 A 都不满足（DPDK 反而慢），必然是 wiring bug（用户之前 Phase 10 就是这种状态）。严苛到 B/C 可能误报。Gate 打印警告 "DPDK slower than kernel by X ns — likely bug" 但不失败，交给人判断是不是真 bug 还是 tail 抖动。
- **验收**：subagent 跑完后在报告里给出 6 组 kernel vs dpdk 的 p50 对比表，tcp/udp/ws 三组必须 DPDK p50 < kernel p50 否则 BLOCKED。

### D-7: 改 kernel 侧的 `print_report` 调用会不会引起 Phase 10 的 output golden 测试失败？

- **问题**：Phase 10 有没有把 `print_report` 的 exact stdout 当作 golden 比较？
- **决策**：检查 `tests/unit/bench/` 下是否有 `test_print_report.cpp` 对 stdout 做逐字比较。如果有，就把该测试的 expected 文本一起更新。如果没有（只有 API 测试），则无需动。
- **验收**：subagent 执行前必须 grep `tests/unit/bench/` 确认 print_report 的断言点，如有必要在同一 commit 里更新。

---

## 一致性检查

- ✅ D-1 (共享 DpdkBenchEnv) 与 In scope (不复制代码) 一致
- ✅ D-2 (EAL 参数来自 bench.conf) 与 Out of scope (不新增 CLI flag) 一致
- ✅ D-3 (src_port 随机) 与"每次运行独立"的 bench 契约一致
- ✅ D-4 (print_dpdk_config_echo 独立函数) 与"保持 print_report 签名稳定"一致
- ✅ D-5 (argv 合成单测) 与 acceptance gate 7 "单测全绿"一致
- ✅ D-6 (fairness gate) 与 CLAUDE.md "fair comparison" doctrine 一致
- ✅ 6 个 scenario 的 DPDK 分支结构统一 → 对未来新增 scenario 有模板效应
- ✅ 所有改动均在 benchmarks/latency/ 和其 core/ helper 内，不触碰 eph-net-dpdk 或 eph-dpdk 源代码，风险面小
- ✅ kernel 分支的 `print_report` 调用一并更新传入 wall_time_ns，保证 kernel vs dpdk 输出格式对齐（不留 skew）

---

## 执行说明

本 plan 配合 `/repeat` skill 以 subagent 模式执行。subagent 命令：

```
/design auto 按 .artifacts/plan-phase-11-dpdk-measurement-20260411-082123.md
执行阶段 11.0。严格遵循 plan：不引入 plan 外的文件/字段；6 个 scenario 的
DPDK 分支结构字面统一；commit prefix bench(phase-11.0):。环境：gcc14 wrapper
在 /tmp/gcc14-wrap/，export PATH=/tmp/gcc14-wrap:$PATH 后用 xmake。完成后
运行完整 8 条 verification gate（build / kernel regression / dpdk live /
fairness / report completeness / config echo / unit tests / deliverable
checklist）并在 commit body 报告每条结果。如 fairness gate 未通过 → STATUS:
BLOCKED，不要自行放宽阈值。
```
