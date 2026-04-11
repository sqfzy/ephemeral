# Plan: Phase 11.1 — TX/RX leg decomposition + Recorder::export_json

> 给 `lat_{tcp,udp,ws,ex_order,ex_md_udp}` 增加 TX / RX leg 记录（`lat_ex_market`
> 保持 1-way），所有 scenario 把 `eph::utils::Recorder::export_json` 结果落到
> `benchmarks/latency/outputs/` 下。单 sub-phase，subagent 执行，单 commit。

创建时间：2026-04-11
状态：已确认

---

## 定位与边界

**目标**：让 latency bench 的输出信息量对齐 pre-v3.3 baseline（RTT + TX + RX 三 leg），同时把 JSON 结果落盘供后续对比/绘图使用。

**用户**：bench 运行者需要区分 "客户端→mock wire 时延"（TX）和 "mock→客户端 wire 时延"（RX），以判断 DPDK 在哪个方向上收益更大。

**In scope**：
- 新协议：payload 头 24 B 时戳块（LE uint64 × 3：`T_client_send / T_mock_recv / T_mock_send`）用于 raw tcp/udp/ws 和 ex_md_udp
- `lat_ex_order`：因 payload 是 WS JSON，走 JSON 字段路径 `t_client / t_mock_recv / t_mock_send`
- `lat_ex_md_udp`：**回退到 RTT 模式**（Phase 10 D-1 的 1-way push 决策在本 phase 被明确撤回）；client 发送 mold64 类似帧，mock 回传
- 5 个 scenario 并行维护 3 个 `Recorder`（`rec_rtt` / `rec_tx` / `rec_rx`），样本算完三段后分别 `record_ns`
- `lat_ex_market` 保持 1-way push，只有 1 个 `rec_oneway` Recorder
- 所有 6 个 scenario 在 teardown 时调 `Recorder::export_json("benchmarks/latency/outputs")`，每 leg 一个 JSON 文件
- `benchmarks/latency/outputs/` 加入 `.gitignore`
- `core/measurement.hpp`: 新增 `print_leg_report(scenario, backend, rtt, tx, rx, warmup, wall_ns)` 覆盖 4-block 打印（RTT/TX/RX + 配置行）；原 `print_report` 保留给 `lat_ex_market` 单 leg 用
- Python mock 对应改造（5 个文件：tcp_echo, udp_echo, ws_echo, ex_order_echo, ex_md_udp_push → ex_md_udp_echo）

**Out of scope**：
- 不记录 SRV leg（mock 内部开销），虽然时戳块里的 `T_mock_send - T_mock_recv` 可以算出来，但用户只要 TX/RX，保持数据项最少
- 不改 `Recorder` 源代码（它已经提供 `export_json`）
- 不动 bench.conf schema
- 不改 lat wrapper 脚本
- 不引入新的 codec；ex_md_udp 继续用 `RawDatagramCodec`，client 发送的首 24 B 就是时戳块
- 不做 N-way 多连接测试
- 不生成聚合报告 HTML/PNG（用户只要 JSON 落盘）

**Non-goals**：
- 不追求 ex_md_udp 真的像 ITCH/MOLD 一样是多播 + 顺序号；改成 RTT 后它就是"带 24B 时戳前缀的 UDP echo"，语义和 lat_udp 非常接近，差异仅在 payload 结构（mold 外层 + 内层 T field 可以全部保留作为 payload 内容，只是在最前面叠加一个 24B 时戳块）

---

## 技术选型

| 类别 | 选择 | 理由 |
|---|---|---|
| 时戳协议 | 24 B LE 前缀 `[T_client\|T_mock_recv\|T_mock_send]` | 简单、对所有二进制 payload 一致；Python `struct.pack('<Q', ...)` 直接支持；payload ≥ 256 B 时占比 < 10% |
| ex_order 时戳 | JSON 字段 `t_client` / `t_mock_recv` / `t_mock_send`（uint64 ns） | 二进制前缀塞不进 WS JSON，直接加 JSON 字段最自然；客户端用 `core/json_scan.hpp`（已有 `scan_json_uint_field`）提取 |
| Python 时戳读取 | `_clock.monotonic_raw_ns()` 已有 | 无须新依赖 |
| Recorder 命名 | `lat_<scenario>_<backend>_rtt` / `_tx` / `_rx` / `_oneway` | `Recorder::export_json` 用 `name` 做文件名前缀，不同 leg 必然落到不同 JSON |
| 输出目录 | `benchmarks/latency/outputs/` (相对 CWD) | 和 `Recorder::export_json` 默认行为一致，.gitignore 忽略 |
| wall_time | `monotonic_raw_ns() - t_measure_start` | 和 11.0 已有的 throughput 打印逻辑复用 |

---

## 架构设计

### 模块划分

| 文件 | 类型 | 职责 |
|---|---|---|
| `benchmarks/latency/core/timestamp_proto.hpp` | **NEW** | 24 B 时戳协议编解码 helpers（`write_client_ts`, `read_mock_ts`, `TimestampBlock` POD） |
| `benchmarks/latency/core/measurement.hpp` | EDIT | 新增 `print_leg_report(...)` + `export_legs(rec_rtt, rec_tx, rec_rx)` 封装 |
| `benchmarks/latency/tcp/lat_tcp.cpp` | EDIT | 两分支（kernel+dpdk）都改：3 Recorder，时戳前缀发送，recv 后解析 mock 时戳，三 leg record |
| `benchmarks/latency/udp/lat_udp.cpp` | 同 | 同 |
| `benchmarks/latency/ws/lat_ws.cpp` | 同 | 同；WS payload 前 24 B 就是时戳块 |
| `benchmarks/latency/exchange/lat_ex_market.cpp` | EDIT 少量 | 仅改 JSON export 路径；不做 TX/RX 拆（1-way） |
| `benchmarks/latency/exchange/lat_ex_order.cpp` | EDIT | 3 Recorder，payload 是 JSON，时戳走 `scan_json_uint_field` 提取 |
| `benchmarks/latency/exchange/lat_ex_md_udp.cpp` | EDIT 大 | **从 1-way push 改回 RTT**：client 发送（带 24B 时戳前缀 + 空 mold payload），mock 反射 |
| `benchmarks/latency/mocks/tcp_echo.py` | EDIT | 收到后原地覆盖 bytes[8:16]=T_mock_recv, bytes[16:24]=T_mock_send，再 sendall |
| `benchmarks/latency/mocks/udp_echo.py` | 同 | 同 |
| `benchmarks/latency/mocks/ws_echo.py` | 同 | 在 WS frame payload 的前 24 B 做同样的原地覆盖 |
| `benchmarks/latency/mocks/ex_order_echo.py` | EDIT | 解 JSON → 加 `t_mock_recv`/`t_mock_send` 字段 → 序列化回传 |
| `benchmarks/latency/mocks/ex_md_udp_push.py` | **RENAME → `ex_md_udp_echo.py`** | 重写为 echo mock：收 UDP → 回写时戳 → sendto 源地址。原 push 模式删除 |
| `benchmarks/latency/mocks/ex_market_push.py` | EDIT 少量 | 保持 push-only；在 WS frame payload 的前 8 B 塞 `T_mock_send`（client 只测 `now - T_mock_send` 作为 oneway） |
| `benchmarks/latency/lat` | EDIT 少量 | 更新 ex_md_udp 的 mock 启动命令（文件名变了） |
| `.gitignore` | EDIT | 添加 `benchmarks/latency/outputs/` |

### 核心抽象

**`benchmarks/latency/core/timestamp_proto.hpp`**（约 80 LOC）：

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include <span>

namespace bench {

/// 24-byte timestamp block embedded at the start of every RTT-scenario
/// payload. LE uint64 x3. Client writes `client_ns` and zeros the other
/// two before send; mock overwrites `mock_recv_ns` on recv and
/// `mock_send_ns` just before send; client reads all three on recv.
struct TimestampBlock {
    uint64_t client_ns;     // set by client before send
    uint64_t mock_recv_ns;  // set by mock on recv
    uint64_t mock_send_ns;  // set by mock before send
};
static_assert(sizeof(TimestampBlock) == 24);

inline constexpr std::size_t kTimestampBlockSize = sizeof(TimestampBlock);

/// Write the client timestamp into the first 8 bytes of `buf` and zero
/// bytes [8, 24). Pre-condition: buf.size() >= 24.
inline void write_client_ts(std::span<uint8_t> buf, uint64_t client_ns) noexcept {
    TimestampBlock b{client_ns, 0, 0};
    std::memcpy(buf.data(), &b, sizeof(b));
}

/// Read the 24-byte block from the start of `buf`. Returns the block
/// by value. Pre-condition: buf.size() >= 24.
[[nodiscard]] inline TimestampBlock read_ts(std::span<const uint8_t> buf) noexcept {
    TimestampBlock b{};
    std::memcpy(&b, buf.data(), sizeof(b));
    return b;
}

/// Given a full TimestampBlock and the client's recv-complete time,
/// compute the three legs. Returned as uint64_t ns.
struct LegTimings {
    uint64_t rtt_ns;
    uint64_t tx_ns;
    uint64_t rx_ns;
};

[[nodiscard]] inline LegTimings compute_legs(
    const TimestampBlock& ts, uint64_t client_recv_ns) noexcept
{
    return LegTimings{
        .rtt_ns = client_recv_ns - ts.client_ns,
        .tx_ns  = ts.mock_recv_ns - ts.client_ns,
        .rx_ns  = client_recv_ns - ts.mock_send_ns,
    };
}

} // namespace bench
```

**`core/measurement.hpp` 新增**（约 60 LOC）：

```cpp
/// Print a 3-leg report. Each leg prints the same fields as the existing
/// 1-leg print_report (samples, min/p50/p90/p99/p99.9/max/avg/stddev).
/// Additionally prints throughput (samples/s) computed from rec_rtt.
inline void print_leg_report(
    std::string_view scenario_name,
    std::string_view backend,
    eph::utils::Recorder& rec_rtt,
    eph::utils::Recorder& rec_tx,
    eph::utils::Recorder& rec_rx,
    uint64_t warmup_discarded,
    uint64_t wall_time_ns) noexcept;

/// Export all three leg Recorders to `benchmarks/latency/outputs/`.
/// Returns true iff all three export_json calls succeeded.
[[nodiscard]] inline bool export_legs(
    eph::utils::Recorder& rec_rtt,
    eph::utils::Recorder& rec_tx,
    eph::utils::Recorder& rec_rx,
    const std::string& output_dir = "benchmarks/latency/outputs") noexcept;
```

`print_leg_report` 内部 format（稳定 ASCII）：

```
=== lat_tcp (dpdk) ===
samples: 1234567 (warmup 1000 discarded)
RTT_ns:
  min    = 18219
  p50    = 18871
  p90    = 19300
  p99    = 21050
  p99.9  = 24100
  max    = 55000
  avg    = 18950
  stddev = 450
TX_ns:
  min    = ...
  ...
RX_ns:
  min    = ...
  ...
throughput: 123456 samples/s
```

### 数据流（lat_tcp dpdk 分支示例）

```
main:
  load_globals / load_scenario / load_dpdk_env / DpdkPoller + DpdkTcpStream create
  rec_rtt("lat_tcp_dpdk_rtt")
  rec_tx("lat_tcp_dpdk_tx")
  rec_rx("lat_tcp_dpdk_rx")
  install_signal_handler()
  print config echo
  for sample in 0..duration:
    t0 = monotonic_raw_ns()
    bench::write_client_ts(span{payload, 24}, t0)  // bytes[0..24] = TS block
    stream->send(payload)
    while rx_bytes < payload_size: poller->poll()
    t1 = monotonic_raw_ns()
    auto ts   = bench::read_ts(span{recv_buf, 24})  // mock 已回填 bytes[8..24]
    auto legs = bench::compute_legs(ts, t1)
    if sample >= warmup:
      rec_rtt.record_ns(legs.rtt_ns);
      rec_tx .record_ns(legs.tx_ns);
      rec_rx .record_ns(legs.rx_ns);
  print_leg_report("lat_tcp", "dpdk", rec_rtt, rec_tx, rec_rx, warmup, wall_ns)
  export_legs(rec_rtt, rec_tx, rec_rx)
  teardown
```

`lat_ex_order` 差异：时戳协议走 JSON 字段：

```
send: {"req_id": N, "t_client": <ns>, "action": "..."}
recv: {"req_id": N, "t_client": <ns>, "t_mock_recv": <ns>, "t_mock_send": <ns>, ...}
```

客户端用已有 `bench::scan_json_uint_field(payload, "t_client")` / `"t_mock_recv"` / `"t_mock_send"` 提取。

### ex_md_udp 重写要点（C 选项）

- 删除 push-loop。`mocks/ex_md_udp_echo.py`（新名）是简单 UDP echo：`recvfrom → 原地覆盖 bytes[8:16,16:24] → sendto`
- 客户端像 lat_udp 一样发送（payload 前 24 B 是 TS block，后面填随机 mold64-ish bytes 保持 payload_size），recv 后计算 3 leg
- bench.conf `[lat_ex_md_udp]` 去掉 `push_rate_hz` / `msg_per_packet`，改为 `payload_size` / `duration_seconds` / `port`（和 lat_udp 结构一致）
- 重命名后 `lat` 脚本里的启动命令也要改

### 时戳协议的 payload 约束

- payload_size **必须** ≥ 24 B；若 < 24，client 返回 error
- payload_size == 24 B 等同于"只有时戳块，没有业务数据"，是合法的最小化场景
- Client 发送的 bytes[24..payload_size] 保持原来的 0xAB 填充（tcp/udp/ws）或 mold64-ish 填充（ex_md_udp）
- Mock **不能** 改动 bytes[24..]，只能覆盖 bytes[8..24]

---

## 接口设计

### 公共 API

**NEW**:
- `bench::write_client_ts(span, client_ns)`
- `bench::read_ts(span) → TimestampBlock`
- `bench::compute_legs(ts, recv_ns) → LegTimings`
- `bench::print_leg_report(name, backend, rec_rtt, rec_tx, rec_rx, warmup, wall_ns)`
- `bench::export_legs(rec_rtt, rec_tx, rec_rx, dir) → bool`

**改动**:
- `bench::print_report` 签名不变（仍然给 `lat_ex_market` 用）；内部新增一个辅助函数把 Stats 打印成 block（`print_stats_block(label, s)`），`print_report` 和 `print_leg_report` 共用该 helper

**新增 JSON schema**（Recorder::export_json 已有，不改 eph-utils）：

```
benchmarks/latency/outputs/
  lat_tcp_dpdk_rtt_<iso8601>.json
  lat_tcp_dpdk_tx_<iso8601>.json
  lat_tcp_dpdk_rx_<iso8601>.json
  lat_tcp_kernel_rtt_<iso8601>.json
  lat_tcp_kernel_tx_<iso8601>.json
  lat_tcp_kernel_rx_<iso8601>.json
  ... 每个 (scenario, backend) 组合三文件
  lat_ex_market_dpdk_oneway_<iso8601>.json    ← 只有 1 个
  lat_ex_market_kernel_oneway_<iso8601>.json   ← 只有 1 个
```

共 `5 scenarios × 2 backends × 3 legs + 1 scenario × 2 backends × 1 leg = 32` 个 JSON。

### 错误体系

- `TimestampBlock` 读写不返回 error（前置条件检查）
- `export_legs` 任一 leg 失败返回 false；scenario 打 WARN 但继续退出码 0
- payload_size < 24 → scenario 启动时 stderr + return 1
- Mock 侧 recv 数据量 < 24 → Python `logging.warning` + 丢弃该包

---

## 编码规范

- `TimestampBlock` 用 `std::memcpy` 而非 reinterpret_cast（严格别名安全）
- Python mock 用 `struct.pack_into('<Q', buf, offset, ns)` 原地覆盖，避免重新分配
- 所有新增 C++ 代码保持 `noexcept` 且 `[[nodiscard]]` 
- 不打 INFO 日志记录每个样本；TRACE 级可以，但默认编译掉
- `outputs/` 目录由 `Recorder::export_json` 自己 mkdir，scenario 无需预先创建

---

## 实施计划

**Commit 策略**：单 commit，message 头 `bench(phase-11.1): TX/RX leg decomposition + Recorder::export_json`。body 列出文件清单 + 每条 gate 的结果 + 旧 baseline 对比表。

### 阶段 11.1: TX/RX legs + JSON export

**交付物**:

1. **NEW** `benchmarks/latency/core/timestamp_proto.hpp`
2. **EDIT** `benchmarks/latency/core/measurement.hpp`（print_leg_report + export_legs + print_stats_block helper）
3. **EDIT** 5 个 RTT scenario 的 kernel 和 dpdk 两个分支（共 10 个分支）：3 Recorder + 时戳协议 + export_legs
4. **EDIT** `lat_ex_market.cpp` 两个分支：单 Recorder 加 `export_json` 调用（使用新的 1-leg print_report 签名）
5. **EDIT** 5 个 mock（tcp/udp/ws/ex_order）+ **RENAME+REWRITE** `ex_md_udp_push.py → ex_md_udp_echo.py`
6. **EDIT** `ex_market_push.py`：在 WS payload 前 8 B 塞 `T_mock_send`
7. **EDIT** `benchmarks/latency/lat`：ex_md_udp 的 mock 命令改名
8. **EDIT** `benchmarks/latency/bench.conf`：`[lat_ex_md_udp]` 改为 port/payload_size/duration_seconds 三项
9. **EDIT** `.gitignore`：添加 `benchmarks/latency/outputs/`

**验收 gate**（9 条）:

1. **Build**: `xmake build -g benchmarks && xmake build -g tests` 绿
2. **Kernel live**: 6 scenarios 在 bench_ns 路径下跑完，每个产生正确数量的 JSON 文件（tcp/udp/ws/ex_order/ex_md_udp 各 3，ex_market 各 1）
3. **DPDK live**: 6 scenarios 在 vfio-pci 路径下跑完，同上
4. **Fairness**: tcp/udp/ws 三项 DPDK RTT p50 < kernel RTT p50
5. **TX+RX sanity**: 每个 RTT scenario 的 stdout 必须同时出现 `RTT_ns:` `TX_ns:` `RX_ns:` 三段；且 `TX p50 + RX p50 ≤ RTT p50 * 1.1`（允许 10% 误差，tolerate mock 侧 overhead 使三者不严格相加）
6. **JSON sanity**: `ls benchmarks/latency/outputs/*.json | wc -l` ≥ 32；随机抽一个文件用 `python -c "import json,sys; json.load(open(sys.argv[1]))"` 能解析；字段齐全（name / timestamp / samples / latency_ns）
7. **Baseline regression**: 和 Phase 11.0 的数据对比，tcp/udp 的 RTT p50 不得比 11.0 恶化 5% 以上
8. **Unit tests**: `xmake run -g tests` 全绿；新单测 `test_timestamp_proto` 3 cases 通过
9. **Deliverable checklist**: 上述 9 条全部勾选

**推荐 skill**: `/design auto` (单 subagent)

**预估**: 1 个 subagent 会话（~600 行 C++ + ~200 行 Python + ~30 行 config/xmake，≈30 分钟 wall clock）

---

## 关键决策记录

### D-1: ex_md_udp 回退到 RTT 模式？

- **问题**：Phase 10 D-1 把 ex_md_udp 设计为 1-way push（匹配 ITCH/MOLD 实际语义），但用户要求 TX/RX leg 分解，1-way 没有 TX leg
- **选项**：A 排除 ex_md_udp / B 只记 RX leg / C 回退到 RTT
- **决策**：C
- **理由**：用户明确选 C。与 lat_udp 语义接近但保留 mold64 外层 payload 形状作为测试载体
- **验收**：`lat_ex_md_udp` 的 stdout 打印 `RTT_ns:` `TX_ns:` `RX_ns:` 三段；JSON 输出 3 个文件

### D-2: 时戳协议是 24 B 前缀还是 trailing footer？

- **问题**：时戳块放 payload 头还是尾？
- **选项**：A 头 / B 尾
- **决策**：A
- **理由**：头更容易在 TCP stream reassembly 时早期识别；mock 只需读前 24 B 而不是整个 payload；对 Python `memoryview` 友好
- **验收**：`TimestampBlock` 定义处 + 所有 mock 都操作 `buf[0:24]`

### D-3: ex_order 的时戳走 JSON 字段还是二进制前缀？

- **问题**：ex_order payload 是 WS 文本 JSON，往里塞二进制前缀会破坏 JSON 解析
- **选项**：A JSON 字段 / B 二进制前缀 + base64 / C 双协议
- **决策**：A
- **理由**：保持 payload 合法 JSON，mock 只需 `json.loads / json.dumps`；客户端复用已有的 `scan_json_uint_field` 零堆 scanner
- **验收**：`lat_ex_order` client/mock 发送的 WS frame payload 是合法 JSON 且带三个时戳字段

### D-4: Recorder 命名法

- **问题**：`export_json` 用 recorder `name` 做文件前缀；5 scenario × 2 backend × 3 leg = 30 个 Recorder 要有唯一名
- **决策**：`lat_<scenario>_<backend>_<leg>`（e.g. `lat_tcp_dpdk_rtt`）
- **验收**：`outputs/` 下有 32 个 JSON 文件且前缀唯一

### D-5: `export_legs` 失败是硬失败还是 WARN？

- **问题**：JSON 落盘失败算 bench 失败吗？
- **决策**：WARN + 退出码 0（bench 数据 stdout 已经有了）
- **理由**：磁盘满或权限问题不应该污染 bench 结果；用户拿到 stdout 仍是完整的

### D-6: `ex_market` 的 1-way 时戳怎么传？

- **问题**：ex_market 是 mock push，没有 client send；但用户想要 oneway 的 JSON 输出
- **决策**：mock 在 WS frame payload 前 8 B 写 `T_mock_send`，client 计算 `now_recv - T_mock_send` 作为 oneway
- **注意**：当前 ex_market 已经在 JSON 里带 `T` 字段用来算 oneway。本 phase 不改 JSON，只改 Recorder name 和 export path
- **验收**：`lat_ex_market_{kernel,dpdk}_oneway_*.json` 能生成且字段齐全

### D-7: payload_size < 24 如何处理？

- **决策**：scenario main 启动时检查，< 24 直接 stderr + return 1
- **理由**：fail fast；默认 payload 都是 256，不会触发；用户手动设小值是自找麻烦

---

## 一致性检查

- ✅ D-1 (ex_md_udp 回退 RTT) 与 In scope + Out of scope 的 "不做 N-way 多播" 一致
- ✅ D-2 (头部时戳) 与 "不改 codec" 一致（codec 只看字节流，不区分头尾）
- ✅ D-3 (JSON 字段) 与 "不新增 codec" 一致（仍然 WsCodec + 文本 JSON）
- ✅ D-4 (命名) 与 Recorder::export_json 现有行为一致，无 eph-utils 改动
- ✅ D-5 (WARN) 与 "bench 写无文件硬依赖" 的既定设计一致（stdout 是主输出）
- ✅ D-6 (push 时戳) 向下兼容 current ex_market JSON T field
- ✅ D-7 (payload 下限) 与 fail-fast 原则一致
- ✅ fairness gate 继续 tcp/udp/ws 硬性要求，没放宽
- ✅ 和 Phase 11.0 一致：所有 6 scenario 的 DPDK 分支结构字面统一（现在是 3 Recorder + 时戳协议 + export_legs）

---

## 执行说明

配合 `/repeat` subagent 模式执行。命令示例：

```
/design auto 按 .artifacts/plan-phase-11.1-tx-rx-legs-json-export-20260411-093213.md
执行阶段 11.1。严格遵循 plan：不引入 plan 外的文件/字段；所有 RTT scenario
的结构字面统一；ex_md_udp 重命名+重写 mock；commit prefix bench(phase-11.1):。
环境：gcc14 wrapper 在 /tmp/gcc14-wrap/，export PATH=/tmp/gcc14-wrap:$PATH。
完成后运行 9 条 gate，fairness gate (tcp/udp/ws DPDK p50 < kernel p50) 失败
→ STATUS: BLOCKED，不放宽。TX+RX sanity gate (TX p50 + RX p50 ≤ RTT p50 * 1.1)
失败 → BLOCKED。JSON 数量 < 32 → BLOCKED。
```
