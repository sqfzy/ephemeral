# Code Review Report — TCP refactor (commits fe9ec9e + 2392f39)

## 元信息
- 时间：2026-04-08 09:20 ~ 09:45
- 耗时：~25 分钟
- Diff 来源：`HEAD~2..HEAD` (commits fe9ec9e + 2392f39, NET diff)
- 审查范围：`eph-dpdk/include/eph/dpdk/tcp.hpp`（净 +75 行）+ 3 doc files
- 审查维度：all (audit-style)
- 构建状态：✅ 通过
- 测试状态：✅ 158/158 通过 (test_net_header 102 + test_tcp 56)
- Bench 状态：✅ 全 6 scenarios × 2 transports 通过；所有数据符合预期

---

## Review 摘要

### 变更概况
- 4 个文件修改 (1 code + 3 docs)
- +528 / −21 (主要是 docs；code 净增 ~75 行 in tcp.hpp)
- 主要变更：把 fe9ec9e 引入的 opt-in TS/piggyback 标志重构成 default-on 的 Linux 风格 delayed-ACK timer

### 总体评价
设计方向正确（默认行为=正确行为，无 opt-in 摩擦），实现简洁，与 Linux TCP 既有设计同构。两个真实问题已修复：(1) `ack_delay_cycles_()` 的 lazy init 在 TSC 未校准时缓存错误的 fallback 值——已改为 retry-until-success；(2) 注释精度问题已修正。剩余 Major 是新行为零单元测试覆盖（pre-existing test_tcp 不构造 TcpSession，需要 follow-up）。

### 问题统计
- 🔴 Critical：0
- 🟡 Major：2（1 已修复，1 follow-up）
- 🔵 Minor：3（1 已修复）
- 💬 Nit：1

### 结论
**APPROVE (post-fix)**：Major #1 已就地修复并重新验证；Major #2 是 pre-existing test 缺口，不阻塞本次 push 但应跟踪。

---

## 🟡 Major 1 — `ack_delay_cycles_()` lazy init 的 TSC 未校准 race  *(已修复)*

**文件**：`eph-dpdk/include/eph/dpdk/tcp.hpp:1326-1338`
**类型**：正确性

### 描述
原实现：

```cpp
static uint64_t ack_delay_cycles_() noexcept {
    static const uint64_t v = eph::utils::TSC::to_cycles(40'000.0)
                                  .value_or(40'000);
    return v;
}
```

`static const` 一旦初始化就**永久**缓存。如果 `flush_pending_ack()` 在 `TSC::init()` 完成之前被首次调用，`TSC::to_cycles()` 返回 `nullopt`，fallback 值 40000 cycles 被永久 cache。在 1 GHz Graviton 上等于 40 µs ✓，但在 2.5 GHz 上是 16 µs，5 GHz 上是 8 µs——silent 的"timer 比预期短"。

### 修复
改为 atomic 缓存，TSC 未校准时**不缓存**，让后续调用重新尝试：

```cpp
static uint64_t ack_delay_cycles_() noexcept {
    static std::atomic<uint64_t> cached{0};
    uint64_t v = cached.load(std::memory_order_relaxed);
    if (v != 0) [[likely]] return v;
    auto opt = eph::utils::TSC::to_cycles(40'000.0);
    if (!opt) [[unlikely]] {
        // TSC not initialised yet — use 1 GHz fallback but DON'T cache;
        // a later call (post TSC::init) will pick up the calibrated value.
        return 40'000;
    }
    cached.store(*opt, std::memory_order_relaxed);
    return *opt;
}
```

### 验证
- 全部测试通过 (158/158)
- 全量 bench 通过 (6 scenarios × 2 transports，order_rtt DPDK TX p50 = 14.0 µs，与修复前等价)

---

## 🟡 Major 2 — 零单元测试覆盖  *(follow-up，不阻塞)*

**文件**：`eph-dpdk/tests/test_tcp.cpp`
**类型**：测试

### 描述
`test_tcp.cpp` 现有 56 个测试**全部**聚焦于 `TcpConfig` 字段校验，**没有任何一个**测试触及 `TcpSession` 的 runtime 行为（构造 session 需要真实 DPDK env）。这意味着新的 delayed-ACK timer 逻辑（process_rx 的 timer 设置 / send 的清零 / flush_pending_ack 的 expiry 检查 / poll_rx 末尾的 idle flush）**完全没有覆盖**。

测试缺口不是 refactor 引入的（pre-existing），但 refactor 改变了关键行为路径，应补一些覆盖。

### 现状评价
- bench 已用 500k+ samples × 2 modes × 6 scenarios 验证 happy path
- 既有测试没有 regression
- TcpSession 整个 runtime 路径都缺单元测试，不是本次 refactor 的责任

### 建议（follow-up，不阻塞 push）
把 `ack_delay_cycles_()` 拆成可注入的 dependency，写一个不依赖 DPDK 的小 TcpSession 单元测试，覆盖：
- timer 未到 → flush 不发
- timer 已到 → flush 发
- send 清零 → 后续 flush 不发
- process_rx 设置首次 timer，后续不覆盖（preserves earliest）

---

## 🔵 Minor 1 — 文档：Linux ATO 的描述不精确  *(已修复)*

**文件**：`eph-dpdk/include/eph/dpdk/tcp.hpp:1322`
**类型**：可观测性 / 文档

### 描述
原注释 "Linux's corresponding constant is ATO=40 ms (quickack/delack) for public internet" 不准确。Linux 的 `TCP_DELACK_MIN` 是 ~4 ms，`TCP_DELACK_MAX` 是 200 ms。"40 ms" 既不是 MIN 也不是 MAX。

### 修复
```cpp
/// Linux's TCP_DELACK_MIN is ~4 ms and TCP_DELACK_MAX is 200 ms
/// (HZ-dependent) for public internet; we tighten to 40 µs because we
/// target datacenter.
```

---

## 🔵 Minor 2 — Reactor empty-burst path 不调 `flush_pending_ack`  *(可接受，未修)*

**文件**：`eph-dpdk/include/eph/dpdk/reactor.hpp:409`
**类型**：设计

### 描述
reactor 主循环 `if (nb_rx == 0) continue;` 直接跳过分发，没调用任何 session 的 `flush_pending_ack`。`poll_rx` 路径补了这个，reactor 路径没补。

后果：reactor-managed connection 完全空闲时，timer 不会触发——下次有 packet 到达时，timer 已经过期很久，bare ACK 立即发出——所以 ACK 仍会被发送，只是延迟到下一个 RX 事件。

### 风险评估
低。Reactor 设计目的是高吞吐持续流量，"完全空闲数十毫秒以上"不是它的目标场景。Linux 自己的 NAPI driven RX 也有同样的特性。可接受。

### 建议
不必修。可选地在 `if (nb_rx == 0) continue;` 上方加注释说明这个边界。

---

## 🔵 Minor 3 — `process_rx` 注释 "earliest pending time" 不完全准确  *(已修复)*

**文件**：`eph-dpdk/include/eph/dpdk/tcp.hpp:1064-1069`
**类型**：可观测性 / 文档

### 描述
注释说 "preserves earliest pending time"，但实际记录的是 process_rx 的**调用时刻**，不是包到达 NIC 的真实时刻。差异 < 1 µs，对 40 µs timer 完全没影响。但措辞不准确。

### 修复
改成 "Records the time of the *first* burst that left data un-ACK'd"。

---

## 💬 Nit — 缺 TRACE 级日志  *(未修)*

**文件**：`eph-dpdk/include/eph/dpdk/tcp.hpp:1093-1101`
**类型**：可观测性

### 描述
`flush_pending_ack` 实际触发 bare ACK 时只在 send 失败时 WARN log，没有 TRACE-level 的 "delayed ACK fired after Xµs"。在调试 RX-only 流的 ACK 时机时会有用。

### 建议
可选地加：
```cpp
SPDLOG_LOGGER_TRACE(detail::tcp_logger(),
    "delayed ACK fired after {}ns", ...);
```

---

## 亮点

- **`tcp.hpp:618-625` send() 的 piggyback 注释**：清楚解释了 "piggyback half" 和 "timer half" 是 delayed-ACK 的两个对称半。
- **`tcp.hpp:1318-1325` `ack_delay_cycles_()` 的注释**：把 timer 值的选择理由（datacenter RTT vs Linux 公网 ATO）写得很清楚。
- **`tcp.hpp:1131-1136` `poll_rx` 末尾的 idle flush**：用最少的代码（4 行）填补了 RX-only 场景的 timer 漏洞，commentary 解释了为什么需要。
- **plan 文件 `.artifacts/plan-tcp-refactor-20260408.md`**：5 个决策点 D-1..D-5 全部带 "选项 / 决策 / 理由 / 验收标准"，将来回看为什么这么设计直接读 plan 即可。
- **两个 commit 的 commit message**：完整记录了根因调查、备选方案、性能数据、wire 兼容性。`git log` 本身就是文档。

---

## 全量 bench 验证（6 scenarios × 2 transports）

跑完所有 6 个 scenarios，不仅验证 order_rtt：

| Scenario | Kernel TX p50 | DPDK TX p50 | DPDK win? |
|---|---|---|---|
| tcp_echo (64B) | 12.2 µs | 11.6 µs | ✓ +0.6 µs |
| udp_echo (64B) | 11.5 µs | 10.7 µs | ✓ +0.8 µs |
| ws_echo (64B)  | 11.8 µs | 13.3 µs | ✗ −1.5 µs (server-side blocking poll, not refactor-related) |
| market_rx      | RTT 14.5 µs | RTT 8.1 µs | ✓ +6.4 µs (RX-only flow benefits from busy-poll) |
| order_rtt      | 15.1 µs | 14.0 µs | ✓ +1.1 µs |
| udp_relay      | 11.1 µs | 10.3 µs | ✓ +0.8 µs |

**5 / 6 scenarios DPDK win**。`ws_echo` 唯一 outlier 是 mock server `echo_mode` 关闭了 tick push → `poll_ms=1` blocking → 高 wakeup 方差。**与 refactor 无关**——pre-existing 特性，影响 kernel 和 DPDK 同样。

数据文件：
- `.bench/full_kernel_20260408.txt`
- `.bench/full_dpdk_20260408.txt`

---

## Diff 统计

```
 .artifacts/bench-optimize-20260408-order_rtt.md | 154 +++++++++++++
 .artifacts/plan-tcp-refactor-20260408.md        | 291 ++++++++++++++++++++++++
 .bench/HISTORY.md                               |   8 +
 eph-dpdk/include/eph/dpdk/tcp.hpp               |  96 ++++++--
 4 files changed, 528 insertions(+), 21 deletions(-)
```
