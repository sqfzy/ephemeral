# Design Intuition Audit Report

## 元信息
- 时间：2026-04-13 11:31
- 审计范围：全项目设计直觉性审计（130 headers, ~42K LOC）
- 审查维度：命名语义、层级关系、模块归属、概念一致性

---

## 审计摘要

### 总体评价

ephemeral 的整体架构设计质量很高——concept-driven 零虚派遣、模块依赖单向清晰、零拷贝贯穿始终。但在 v3.3 重构过程中，由于分阶段迁移和历史兼容，积累了若干**命名与归属上违反直觉**的地方。这些问题不影响正确性，但会让新读者困惑。

### 问题统计
- 🔴 Critical：0
- 🟡 Major：3（语义/层级倒置，会系统性误导读者）
- 🔵 Minor：4（不够理想但可理解）
- 💬 Nit：2

### 结论
**NEEDS_DISCUSSION** — 无正确性/安全性问题，但有几个设计命名值得讨论是否值得修正。

---

## Review 条目

---

### 🟡 Major #1: Reactor 是底层、Poller 是上层——与业界语义倒置

**文件**：`eph-net-dpdk/include/eph/dpdk/reactor.hpp` vs `eph-net-dpdk/include/eph/net/dpdk/poller.hpp`
**类型**：设计
**描述**：

业界标准语义：
- **Poller** = I/O 多路复用原语（poll/epoll/kqueue），纯"哪些 fd 就绪"
- **Reactor** = POSA2 设计模式，在 Poller 之上做事件分发 + 回调派发，是面向用户的驱动循环

当前项目：`Reactor<>` 做 DPDK burst 收包（底层），`DpdkPoller` 做 concept 适配（上层）。关系完全反转。

**建议**：
- 方案 A：将 `eph::dpdk::Reactor` 重命名为 `BurstEngine` / `RxDispatcher` / `BurstLoop`，明确它是底层收包引擎而非 Reactor 模式
- 方案 B：将 v3.3 concept 层的 `Poller` 改名为 `Reactor`（但影响面大，kernel 侧也得改）
- 推荐方案 A，因为 `Reactor<>` 本身标注为 legacy，改名成本低

---

### 🟡 Major #2: `eph-core` 中大量类型使用 `eph::net` 命名空间——模块与命名空间错位

**文件**：`eph-core/include/eph/core/tcp_state.hpp`、`framer_concept.hpp`、`length_prefix_framer.hpp`、`error_traits.hpp`、`tcp_concept.hpp`
**类型**：设计
**描述**：

`eph-core` 是最底层的模块，但其中至少 5 个头文件声明的类型在 `namespace eph::net` 下：

| 文件（在 eph-core 中） | 命名空间 | 类型 |
|---|---|---|
| `tcp_state.hpp` | `eph::net` | `TcpState`, `tcp_state_name()` |
| `framer_concept.hpp` | `eph::net` | `FrameError`, `DecodedFrame`, `MessageFramer` concept |
| `length_prefix_framer.hpp` | `eph::net` | `LengthPrefixFramer` |
| `error_traits.hpp` | `eph::net` | 向后兼容 alias |
| `tcp_concept.hpp` | `eph::net` | `TcpTransport` concept（legacy） |

这造成一个反直觉的局面：**include 路径说 `eph/core/`，namespace 说 `eph::net`**。读者看到 `eph::net::TcpState` 会去 `eph-net` 模块找，找不到。

**根因**：Phase 4 为解决 ODR 冲突将定义下沉到 eph-core，但保留了 eph::net 命名空间以免破坏下游代码。

**建议**：
- 短期：在 `eph-core` 的这些头文件顶部加一行注释 `// NOTE: namespace is eph::net despite living in eph-core — see Phase 4 ODR history`（已有，但不够醒目）
- 长期：考虑将这些类型迁移到 `namespace eph::core`，在 `eph-net` 提供 `using` 别名过渡。或者反过来——将这些头文件物理移回 `eph-net`，让 `eph-core` 不再包含任何 `eph::net` 命名空间的东西

---

### 🟡 Major #3: `eph/dpdk/` vs `eph/net/dpdk/` 双目录——层级语义不直观

**文件**：`eph-net-dpdk/include/eph/dpdk/` 和 `eph-net-dpdk/include/eph/net/dpdk/`
**类型**：设计
**描述**：

同一个模块 (`eph-net-dpdk`) 暴露了两个 include 路径前缀：
- `#include "eph/dpdk/tcp.hpp"` — 底层协议栈
- `#include "eph/net/dpdk/tcp_stream.hpp"` — concept 适配层

问题不在于分层本身（分层是对的），而在于：
1. `eph/dpdk/` 这个路径暗示它是一个独立模块 `eph-dpdk`，但实际上它和 `eph/net/dpdk/` 打包在同一个 `eph-net-dpdk` 模块里
2. 用户无法从路径推断出哪个是底层哪个是上层——两个都有 `tcp`、`udp`、`eal` 相关文件
3. `eph/dpdk/` 的命名空间是 `eph::dpdk`，`eph/net/dpdk/` 的命名空间是 `eph::net::dpdk`——前者看起来像 `eph::dpdk` 是个顶级模块

**建议**：
- 方案 A：将 `eph/dpdk/` 移入 `eph/net/dpdk/detail/` 或 `eph/net/dpdk/stack/`，明确它是 `eph-net-dpdk` 的内部实现层
- 方案 B：将 `eph/dpdk/` 拆成真正独立的 `eph-dpdk` 模块（独立 xmake target），`eph-net-dpdk` 依赖它。这更干净但增加了模块数量
- 方案 C（最小改动）：重命名 `eph/dpdk/` → `eph/dpdk_stack/` 或 `eph/dpdk_impl/`，至少从路径上暗示它是实现层而非独立模块

---

### 🔵 Minor #1: `OutputBuffer` 在 `eph::core` 但语义上是网络层概念

**文件**：`eph-core/include/eph/core/codec.hpp`
**类型**：设计
**描述**：

`OutputBuffer` 是 codec decode 时注入自动回复（如 WS pong）的写缓冲。它本质上是网络 I/O 的一部分，但定义在 `eph::core` 里。这是因为 `StreamCodec` concept 引用了它，而 concept 必须在 `eph-core` 定义。

逻辑上没错，但读者看到 `eph::core::OutputBuffer` 会觉得它是某种通用缓冲区抽象，而不是 codec-to-transport 的注入通道。

**建议**：命名改为 `CodecOutputBuffer` 或 `AutoReplyBuffer` 会更清晰地传达其窄用途。

---

### 🔵 Minor #2: `Pollable` concept 的 `poll_once_()` 尾部下划线暗示 private

**文件**：`eph-net/include/eph/net/concepts.hpp`
**类型**：设计
**描述**：

`poll_once_()` 用尾部下划线命名，按 C++ 惯例这暗示"内部使用"。但它是 concept 的必需方法，是 `Pollable` 的公共契约。同样的还有 `is_attached_()`、`native_handle()`、`notify_attached_()`、`notify_detached_()`。

一些带下划线，一些不带，规则不一致。

**建议**：如果意图是"用户不应直接调用，只有 Poller 调用"，可以用注释或文档说明，而不是依赖尾部下划线这种弱信号。或者统一：要么全带要么全不带。

---

### 🔵 Minor #3: `KillSwitch` 和 `g_shutdown_flag` 功能重叠

**文件**：`eph-utils/include/eph/utils/kill_switch.hpp` vs `shutdown_signal.hpp`
**类型**：设计
**描述**：

- `KillSwitch` — 单次触发、不可重置的合规原语
- `g_shutdown_flag` + `install_shutdown_handlers()` — 全局 SIGINT/SIGTERM 处理

两者都是"设置一个 atomic flag 让系统停下来"。区别在于 `KillSwitch` 是应用层主动触发，`g_shutdown_flag` 是信号驱动。但命名上看不出这个区分——新读者会问"什么时候用哪个？"

**建议**：文档或命名上强化区分，例如 `KillSwitch` 改名 `ComplianceKillSwitch` 或在头文件注释中明确"KillSwitch 是业务层合规熔断，shutdown_signal 是进程信号处理"。

---

### 🔵 Minor #4: `StreamConfig` 在 kernel 和 DPDK 中同名但字段不同

**文件**：`eph-net-kernel/include/eph/net/kernel/config.hpp` vs `eph-net-dpdk/include/eph/net/dpdk/config.hpp`
**类型**：设计
**描述**：

两个 backend 都定义了 `StreamConfig`、`UdpConfig`、`PollerConfig`，类型名完全相同但字段集合不同（DPDK 版包装了 legacy `eph::dpdk::TcpConfig`）。由于它们在不同命名空间（`eph::net::kernel` vs `eph::net::dpdk`），技术上没有冲突。但同名不同构的 config 容易在代码审查或跨 backend 迁移时造成混淆。

**建议**：可以接受现状（命名空间足够区分），但如果未来需要 backend-agnostic config，应考虑统一字段集或至少统一必填字段的名称。

---

### 💬 Nit #1: `Reactor<bool>` 的模板参数从未使用

**文件**：`eph-net-dpdk/include/eph/dpdk/reactor.hpp:46`
**类型**：设计
**描述**：`Reactor<bool>` 的 `bool` 模板参数是历史遗留，从未在代码中使用。这让读者困惑——这个 bool 控制什么？答案是什么都不控制。

**建议**：既然 Reactor 是 legacy 且标记为将被淘汰，可以等淘汰时一并处理。如果要保留，移除无用模板参数。

---

### 💬 Nit #2: `detail/` 下的 base64、json_escape 在 eph-core 但只被 eph-net 的 WS 握手使用

**文件**：`eph-core/include/eph/core/detail/base64.hpp`、`json_escape.hpp`
**类型**：设计
**描述**：这些 detail 工具函数定义在 eph-core 但唯一调用者是 eph-net 的 WebSocket 握手代码。放在 eph-core 让它们看起来是"核心基础设施"，实际是 WS 握手的实现细节。

**建议**：考虑移到 `eph-net/include/eph/net/detail/`。不过作为 detail 命名空间下的内容，影响面很小。

---

## 亮点

- **concept 体系设计精良**：`Stream` / `Datagram` / `Poller` 三个 concept 窄而正交，kernel 和 DPDK backend 完全对称实现。用户代码 `auto poller = …; poller->poll()` 在两个 backend 间零改动切换——这是业界罕见的做到位的 concept-driven 设计。
- **PacketView 统一了零拷贝契约**：同一个 WsCodec 不改一行代码就能在 kernel `SpanView` 和 DPDK `MbufView` 上工作，编译期绑定无运行时分支。
- **eph-containers 内聚性极好**：6 个类型全是 SPSC/lock-free 容器，命名清晰（Bounded vs Evicting），无多余抽象。
- **Parser 模块零拷贝一致**：eph-fix、eph-itch、eph-json 全部 string_view 到原始缓冲区，热路径零分配。

---

## 问题总览

| 序号 | 严重度 | 问题 | 核心矛盾 |
|------|--------|------|----------|
| 1 | 🟡 Major | Reactor/Poller 语义倒置 | 底层叫 Reactor，上层叫 Poller，与 POSA2 相反 |
| 2 | 🟡 Major | eph-core 中的 eph::net 命名空间 | 物理模块和逻辑命名空间错位 |
| 3 | 🟡 Major | eph/dpdk/ vs eph/net/dpdk/ 双目录 | 同模块两个 include 前缀，层级关系不透明 |
| 4 | 🔵 Minor | OutputBuffer 命名过于通用 | 实际是 codec 自动回复注入的窄用途缓冲 |
| 5 | 🔵 Minor | poll_once_() 尾部下划线不一致 | concept 公共契约方法用了暗示 private 的命名 |
| 6 | 🔵 Minor | KillSwitch vs g_shutdown_flag 功能重叠 | 命名未体现"合规熔断 vs 信号处理"的区分 |
| 7 | 🔵 Minor | StreamConfig 同名不同构 | 跨 backend 同名 config 字段集不同 |
| 8 | 💬 Nit | Reactor\<bool\> 无用模板参数 | 历史遗留 |
| 9 | 💬 Nit | base64/json_escape 放在 eph-core | 只被 eph-net WS 握手使用 |

---

## 推荐行动

如需修正这些设计问题：
- Major #1 (Reactor 重命名) → `/refactor` 单点重命名，影响面小（legacy 模块）
- Major #2 (namespace 错位) → `/discuss` 先讨论迁移策略，再 `/refactor breaking`
- Major #3 (双目录) → `/discuss` 讨论目标结构，再 `/refactor breaking`
- Minor 级别 → 可随下次相关模块改动顺带修正
