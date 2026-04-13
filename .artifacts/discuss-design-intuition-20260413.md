# Discussion Record

## Context
- 时间：2026-04-13 11:31
- 耗时：约 8 分钟
- 用户原始需求：审计发现 3 个 Major 设计命名/层级问题（Reactor/Poller 语义倒置、eph-core 中 eph::net 命名空间错位、eph/dpdk/ 双目录），讨论修正方案
- 复杂度评估：高
- 讨论轮数：6 轮
- 参与角色：R14 架构师、R6 维护性倡导者、R2 极简主义者、R9 成本审计者、R3 性能狂热者

## 内容摘要

围绕三个设计直觉问题展开讨论。主要争议在改动范围：R2/R6 主张全面修正，R9/R3 主张最小改动。R14 提出将 Issue 2 分为 A 类（TcpState/ErrorEnum，应迁移）和 B 类（framer，因依赖拓扑约束保留），打破僵局。最终共识：Issue 1 (Reactor → RxDispatcher) + Issue 2A (TcpState 等迁到 eph::core) 在一个 PR 完成；Issue 2B 加文档说明；Issue 3 推迟并在 CLAUDE.md 记录为已知妥协。

---

## 最终方案

### 立即执行（一个 PR）

**Issue 1 — Reactor → RxDispatcher**
- `eph::dpdk::Reactor<>` → `eph::dpdk::RxDispatcher`
- 文件 `reactor.hpp` → `rx_dispatcher.hpp`
- 关联类型全部更名（ReactorConfig, ReactorEntry, ReactorDataCallback 等）
- Logger `dpdk.reactor` → `dpdk.rx_dispatcher`
- 影响 ~13 文件

**Issue 2A — TcpState/ErrorEnum 迁到 eph::core**
- `eph/core/tcp_state.hpp`：`namespace eph::net` → `namespace eph::core`
- `eph/core/error_traits.hpp`：ErrorEnum/ErrorEnumFormatter 从 eph::net → eph::core
- `eph/net/tcp_state.hpp` 保留 re-export + `using eph::core::TcpState;`
- 影响 ~10 文件

### 文档标注

**Issue 2B — framer 留 eph-core**
- `framer_concept.hpp`、`length_prefix_framer.hpp` 顶部加文档块说明：物理在 eph-core、namespace 在 eph::net，原因是 eph-fix/itch/json 不依赖 eph-net 但需要 framer

### 推迟

**Issue 3 — eph/dpdk/ 拆为独立 eph-dpdk 模块**
- 在 CLAUDE.md 添加 "Known design compromises" 节记录
- 计划在 Reactor 完全淘汰后正式拆分
- TcpSession 是完整用户态 TCP 栈，不应隐藏在 detail/

---

## 关键论据记录

1. **RxDispatcher 优于 BurstDispatcher**：组件只做 RX 不做 TX；DPDK 社区术语；Burst 是实现细节不应暴露在类名中
2. **Poller concept 不改名**：concept 名称一致性优先于个别实现的语义精确；通过文档补充 DPDK busy-poll 差异
3. **framer 不迁 eph-codec**：传递依赖风险（eph-codec 含 WsCodec 等重量级头文件）；编译搜索路径膨胀
4. **Issue 3 推迟理由**：54 文件 include 路径改动成本高；TcpSession 不是 detail 不应隐藏；header-only target 拆分收益有限
5. **无 ODR 风险**：C++ using 声明不创建新类型，只要迁移在单个 PR 中原子完成
