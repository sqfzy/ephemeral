# Plan: eph-dpdk TCP slow-path fix — clean refactor

> 把 commit fe9ec9e 引入的两个 opt-in 标志拆架走干净，落到设计最优的终态：piggyback ACK 成为默认（带 delayed-ACK timer），TS option 整体删除。

创建时间：2026-04-08
状态：草案

---

## 定位与边界

**目标**：把 fe9ec9e 的"opt-in 临时方案"重构成 long-term 的正确设计。

**触发上下文**：commit fe9ec9e 用两个 opt-in 标志（`enable_timestamps` / `enable_ack_piggyback`）解决了 DPDK TCP 的接收侧 slow path 问题。诊断阶段证伪了 TS option 假设但代码已落地；ACK piggyback 是真正的修复但默认 OFF，调用方需要 opt-in 才能拿到收益。这次重构把临时形态收敛成 clean architecture。

**In scope**：
- 删除 `TcpConfig::enable_timestamps` 及全部 TS option 相关代码（packet_core.hpp 常量与 helper、PacketTemplate 字段、TcpSession::next_tsval_、build_packet 中的 TS 分支）
- 删除 `TcpConfig::enable_ack_piggyback` 字段，piggyback 成为唯一行为
- 在 TcpSession 加 delayed-ACK timer（40 µs 默认），保证 RX-only 工作流不被破坏
- 删除 bench 端 `BENCH_DPDK_TCP_TS` / `BENCH_DPDK_ACK_PIGGYBACK` 两个 env var
- 同步删除 `ConnectorOptions` 上的对应字段
- 更新 commit message 解释行为变更

**Out of scope**：
- 不补完 TS option 的 ts_recent 跟踪（"为没需求功能写代码"，删了更干净）
- 不动 `eph-transport` 任何代码（5 个 flush_pending_ack 调用方用 SFINAE，对行为变化透明）
- 不动既有 158 个测试（grep 已确认它们不引用任何被删/被改的符号）
- 不修复独立的 `bench_latency.sh` Phase 2/3 顺序 bug（另一次的事）
- 不动 `fill_packet`（zero-alloc 路径，本次未涉及）

---

## 技术选型

| 类别 | 选择 | 理由 |
|------|------|------|
| 语言 | C++23（已是项目标准） | 不变 |
| 构建 | xmake（已是项目标准） | 不变 |
| 测试 | 既有 GTest 测试（test_net_header + test_tcp） | 不变，零更新 |
| 时间源 | `eph::utils::TSC`（cntvct_el0，已校准） | 已是 eph-dpdk 标准 |

---

## 架构设计

### 模块划分

| 模块 | 职责 | 改动类型 |
|------|------|------|
| `eph-dpdk/include/eph/dpdk/packet_core.hpp` | TCP option 字节布局常量 + helper | **删除** TS 相关常量和 helper |
| `eph-dpdk/include/eph/dpdk/packet_template.hpp` | TCP packet 构造 | **简化** build_packet（删 enable_timestamps 分支） |
| `eph-dpdk/include/eph/dpdk/tcp.hpp` | TcpSession 状态机 + TcpConfig | **重构**：删两个字段，加 ack_delay timer 状态 + 检查 |
| `eph-dpdk/include/eph/dpdk/connector.hpp` | ConnectorOptions → TcpConfig 透传 | **简化** ConnectorOptions（删两个字段） |
| `benchmarks/latency/framework/ws_transport.hpp` | bench 端 wiring | **删** BENCH_DPDK_TCP_TS 和 BENCH_DPDK_ACK_PIGGYBACK 解析块 |

### 核心抽象

**Delayed ACK timer 模型**（Linux 风格，但 µs 量级）：

```
TcpSession 状态新增（替换原有的 ack_pending_ 单一布尔）：
  uint64_t ack_pending_since_tsc_ = 0;   // 0 = no pending; non-zero = first set time
  static constexpr uint64_t kAckDelayCycles = ...;  // 40 µs in cycles, computed at startup

process_rx() 行为：
  当数据到达且 need_ack=true：
    if (ack_pending_since_tsc_ == 0)
        ack_pending_since_tsc_ = TSC::now();   // 记录第一次"欠一个 ACK"的时刻
    // 不立即发；不调 send_ack()

flush_pending_ack() 新行为：
  if (ack_pending_since_tsc_ == 0) return;     // 无待 ACK，no-op
  if (TSC::now() - ack_pending_since_tsc_ < kAckDelayCycles) return;  // timer 未到，继续延迟
  ack_pending_since_tsc_ = 0;
  (void)send_ack();                            // timer 触发：发 bare ACK

send() 行为（不变于 fe9ec9e 之后）：
  ack_pending_since_tsc_ = 0;                   // 数据 send 已携带最新 cumulative ACK
  // ... build_packet + tx_burst

poll_rx() 末尾新增：
  flush_pending_ack();                          // 即使 0 个 RX 包也检查 timer
                                                // (idle 连接靠用户的 poll 频率反复触发 timer)
```

**关键性质**：
- request/response 模式：每轮 send() 立即清掉 pending → timer 永远不触发 → 0 个 bare ACK 包 → kernel 接收侧走 fast path
- RX-only 流（市场数据订阅）：每个 RX 周期都进 flush_pending_ack → 40 µs 后 timer 触发 → 发 bare ACK → 服务器拿到 ACK，连接健康
- 混合模式：自适应——有 outgoing data 时 piggyback，否则 ≤40 µs 后 bare ACK
- 调用方无感知：所有现有 `flush_pending_ack()` 调用站点行为正确，无需修改

### 数据流

```
                  ┌──────────────┐
   RX 包 ────────▶│ process_rx   │ 设 ack_pending_since_tsc_ 若是首次
                  └──────┬───────┘
                         │
                  ┌──────▼───────┐
                  │ poll_rx 末尾 │ → flush_pending_ack()
                  └──────┬───────┘
                         │
                ┌────────▼─────────┐
                │  timer expired?  │
                └───┬──────────┬───┘
                    │ no       │ yes
                    │          ▼
                    │     send bare ACK
                    │     清 ack_pending_since_tsc_
                    │
            ┌───────▼──────┐
            │  user send() │ 清 ack_pending_since_tsc_
            └──────────────┘    携带 cumulative ACK
```

---

## 接口设计

### 公共 API 变化

**TcpConfig**：
```cpp
// 删除：
- bool enable_timestamps = false;
- bool enable_ack_piggyback = false;

// 不新增任何字段——ack_delay 硬编码为 40 µs，不可配置
// （将来如果需要再开 knob，零开销）
```

**ConnectorOptions**：
```cpp
// 删除：
- bool enable_timestamps = false;
- bool enable_ack_piggyback = false;
```

**TcpSession 公共接口**：完全不变。`flush_pending_ack()` / `send()` / `poll_rx()` 签名不变，只是内部语义升级。

**packet_core.hpp**：
```cpp
// 删除以下常量和函数（commit fe9ec9e 引入的全部 TS 设施）：
- kSynOptionsLenWithTs
- kSynTcpHeaderLenWithTs
- kTcpDataOptionsLenWithTs
- kTcpHeaderLenWithTs
- write_syn_options_with_ts(...)
- write_tcp_ts_option(...)

// 保留（fe9ec9e 之前就有，与本次重构无关）：
- kSynOptionsLen
- kSynTcpHeaderLen
- write_syn_options(...)
```

**PacketTemplate**：
```cpp
// 删除字段：
- bool enable_timestamps = false;
- uint32_t ts_val = 0;
- uint32_t ts_ecr = 0;

// build_packet 的 if-else 退回单一形式（删除 is_syn 内的 enable_timestamps 分支）
// 删除非 SYN 分支的 TS option 写入
```

### 错误体系

不变。

---

## 编码规范

不变（沿用项目既有规范）。新增代码遵循：
- spdlog 日志：DEBUG 级别 log "ACK timer expired, sending bare ACK (delayed Xµs)"
- 注释：解释 *why* 用 delayed ACK（防 RX-only 连接破坏 + 给 piggyback 机会）
- 常量命名：`kAckDelayCycles`（与现有 `kDefaultMss` 等同风格）
- TSC 转换在 namespace 级 inline 函数里完成一次：`inline const uint64_t kAckDelayCycles = TSC::to_cycles(...)` 不行（TSC::init 必须先完成），所以用 lazy init pattern：在 TcpSession 第一次需要时调用 helper

---

## 实施计划

> **Commit 策略**：单 commit，原子重构。重构完后跑 158 测试 + production-config bench 二次验证不退化。

### 阶段 1：核心重构（单 step，单 commit）

**步骤序列**（按文件依赖顺序）：

1. `eph-dpdk/include/eph/dpdk/packet_core.hpp`：
   - 删 `kSynOptionsLenWithTs` / `kSynTcpHeaderLenWithTs` / `kTcpDataOptionsLenWithTs` / `kTcpHeaderLenWithTs`
   - 删 `write_syn_options_with_ts()` / `write_tcp_ts_option()`

2. `eph-dpdk/include/eph/dpdk/packet_template.hpp`：
   - 删 PacketTemplate 的 `enable_timestamps` / `ts_val` / `ts_ecr` 字段
   - 简化 `build_packet`：恢复 `is_syn ? kSynTcpHeaderLen : kTcpHeaderLen`，删 enable_timestamps 分支
   - 删非 SYN 分支里的 TS option 写入

3. `eph-dpdk/include/eph/dpdk/tcp.hpp`：
   - 删 `TcpConfig::enable_timestamps` / `TcpConfig::enable_ack_piggyback` 字段
   - 删 TcpSession 构造函数里 `pkt_template_.enable_timestamps = ...` 等 3 行
   - 删 `next_tsval_()` helper 和 6 处调用
   - 改 `ack_pending_` 类型：`bool` → `uint64_t ack_pending_since_tsc_`（语义：0 表示无 pending；非 0 表示第一次置位的 TSC 时刻）
   - 加 `static const uint64_t kAckDelayCycles` 静态成员（lazy init via 函数局部 static）
   - `process_rx`：把 `if (need_ack) ack_pending_ = true;` 改成 `if (need_ack && ack_pending_since_tsc_ == 0) ack_pending_since_tsc_ = TSC::now();`
   - `send()`：把 `ack_pending_ = false;` 改成 `ack_pending_since_tsc_ = 0;`
   - `flush_pending_ack()`：重写为 timer-aware 版本（见上方"核心抽象"）
   - `poll_rx()` 末尾：在 return 前调一次 `flush_pending_ack()`（保证 idle 连接也能触发 timer）

4. `eph-dpdk/include/eph/dpdk/connector.hpp`：
   - 删 `ConnectorOptions::enable_timestamps` / `ConnectorOptions::enable_ack_piggyback`
   - 删 `prepare_connection` 里 `.enable_timestamps = opts.enable_timestamps,` 和 `.enable_ack_piggyback = opts.enable_ack_piggyback,` 这两行

5. `benchmarks/latency/framework/ws_transport.hpp`：
   - 删整个 `[TS-OPT]` 块
   - 删整个 `[ACK-PIGGYBACK]` 块

**交付物**：单 commit，包含上述 5 个文件改动。

**验收标准**：
- 构建：`xmake build bench_order_rtt bench_order_rtt_dpdk bench_mock_server test_net_header test_tcp` 全部成功
- 测试：`xmake run test_net_header` (102 PASS) + `xmake run test_tcp` (56 PASS) 全部通过
- 行为：无需 env var 即可获得 fe9ec9e 同等性能
- Wire 兼容：tcpdump 显示数据包字节布局与 fe9ec9e 之前的"未启用 TS"路径一致（即 20 字节 TCP header），但每个 round 只有 1 个 C→S 包（无 bare ACK）
- 性能：DPDK TX p50 ≤ 14 µs（与 fe9ec9e + ACK_PIGGYBACK=1 等价或更好）

**推荐 skill**：直接 Edit 工具人工实施（改动量小且精确），无需 /design auto。

**预估**：单会话内完成。

### 阶段 2：验证 + commit

**步骤**：
1. 重构完，rebuild
2. 跑 test_net_header + test_tcp（必须 158/158）
3. 跑 production-config bench 对比：
   - kernel 模式
   - dpdk 模式（**无 env var**）
4. 与 commit fe9ec9e 的最终数据对比，确认未退化
5. tcpdump 验证：DPDK 包不再有 bare ACK；连接握手仍正常
6. 写 commit message，提交

**验收标准**：
- 158/158 测试通过
- DPDK TX p50 与 fe9ec9e ACK_PIGGYBACK=1 模式一致（±0.5 µs 噪声）
- 工作树干净

**预估**：单会话内完成。

---

## 关键决策记录

### D-1: TS option 完全删除而非完成实现
- **问题**：commit fe9ec9e 引入了半成品的 TS option（ts_ecr 永远 0）。完成它 vs 删除它？
- **选项**：A 完全删除 / B 完成 ts_recent 跟踪 / C 维持 opt-in
- **决策**：A 完全删除
- **理由**：诊断阶段已经直接证伪 TS 是 slow path 元凶（perf kprobe 计数：加 TS 后 tcp_data_queue 仍 209k，与不加 TS 一致）。完成 B 是为没有需求的功能投入 ~200 行代码。删除符合 "一步到位" 的设计哲学——不留无用代码。
- **验收标准**：grep 全项目无 `enable_timestamps` / `ts_val` / `ts_ecr` / `kSynOptionsLenWithTs` / `kTcpHeaderLenWithTs` / `kTcpDataOptionsLenWithTs` / `write_syn_options_with_ts` / `write_tcp_ts_option` / `next_tsval_` 任何引用。

### D-2: ACK piggyback 用 delayed-ACK timer 而非 always-on
- **问题**：直接 always-on piggyback 会破坏 RX-only 工作流（市场数据订阅）。怎么兼顾？
- **选项**：A always-on 无 fallback / B delayed-ACK timer / C 保留 opt-in 标志
- **决策**：B delayed-ACK timer
- **理由**：这是 Linux TCP 自己的做法（quick ACK / delayed ACK），不是 hack。timer 让 request/response 拿到全部收益，同时 RX-only 在 ≤40 µs 后自动发 bare ACK，连接保持健康。所有 5 个 `flush_pending_ack` 调用方对行为升级透明（SFINAE 检测）。
- **验收标准**：
  - request/response 测试（order_rtt）：tcpdump 确认 0 个 bare ACK 包；kernel 接收侧 tcp_data_queue 计数 ≤100/5s
  - RX-only 测试（如 market_rx）：连接稳定运行 ≥10 秒不出 RST/重传错误

### D-3: ACK delay 用 40 µs 硬编码而非可配置
- **问题**：timer 值硬编码 vs `TcpConfig::ack_delay` 字段
- **选项**：A 硬编码 40 µs / B 可配置字段
- **决策**：A 硬编码
- **理由**：YAGNI——目前没有任何用例需要不同的值。datacenter 工作负载下 40 µs 远大于典型 RTT (~20 µs)，所以 request/response 永远不触发；同时远小于人类感知 (40 ms)，所以 RX-only 的 ACK 延迟无感。如果将来真的有人需要 tune，加 knob 是零开销改动。提前加 knob 是过度工程。
- **验收标准**：`TcpConfig` 不引入任何新字段。

### D-4: bench 端 env var 全删而非保留 debug
- **问题**：是否保留 `BENCH_DPDK_ACK_PIGGYBACK=0` 给 debug 时禁用？
- **选项**：A 全删 / B 保留禁用开关
- **决策**：A 全删
- **理由**：piggyback 现在是默认且永远是正确行为，timer 已经覆盖 RX-only 边界情况。"禁用 piggyback" 不是有意义的 debug 状态——它只会让 bench 故意触发 50% slow path，没有诊断价值。
- **验收标准**：`benchmarks/latency/framework/ws_transport.hpp` 无任何 env var 解析；脚本调用 `bench_latency.sh --transports dpdk` 不需要任何额外环境变量即可获得最优性能。

### D-5: 单 commit 原子重构而非拆分
- **问题**：单 commit vs 拆 3 个 commit（删 TS / 加 timer / 删 env var）
- **选项**：A 单 commit / B 多 commit
- **决策**：A 单 commit
- **理由**：拆开后中间状态不自洽（例如：删了 ConnectorOptions 字段但 TcpConfig 还有，编译失败）。单 commit ~150 行删除 + ~30 行新增，可控；review 时一次看完整图景比拆开更清晰；回滚也是一次性的。
- **验收标准**：`git log` 显示一个新 commit，`git show` 包含全部 5 个文件改动。
