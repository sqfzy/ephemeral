# TODO

## 大功能

- [ ] **eph-gateway（连接管理层）** — 多交易所连接编排、reconnect 状态恢复、连接优先级、健康检查。高复杂度，3-4h。
- [ ] **UDP multicast receiver** — equity 行情接收（CME MDP3.0、Nasdaq TotalView via MoldUDP64）。`MulticastReceiver<DpdkImpl>` + IGMP join。中复杂度，2-3h。
- [ ] **io_uring 后端** — 填补 socket/DPDK 中间层。极高复杂度，需 kernel API + fallback。**优先级低，暂不实施**。

## 中功能

- [ ] **MetricsSink concept** — 定义 `push(name, value, tags)` 接口，内置 NullSink + ConsoleSink，示例 Prometheus/StatsD 适配器。1-2h。
- [ ] **eph-audit 审计日志骨架** — 基于 mmap ring buffer 的结构化审计日志，监管合规基础（MiFID II / Reg NMS）。2-3h。
- [ ] **Crash handler + kill switch** — 崩溃时撤销未成交订单、dump 状态、通知监控。一键断开所有连接。1-2h。

## 小改进

### 代码质量
- [x] MapBook `double` key 与 ArrayBook `epsilon` 语义不一致 — MapBook 已加 epsilon-tolerant find_approx
- [x] `RateLimiter::available()` 中 `const_cast` — 改为非 const 方法
- [x] `format_mac` 在 `arp.hpp` 和 `tcp.hpp` 中重复 — 合并到 `net_header.hpp`
- [x] 删除 `SharedRxDispatcher`（`shared_rx.hpp`）— 已删除，benchmark 迁移到 Reactor

### 文档
- [ ] `summary.md` 更新 — 反映新模块（eph-core、eph-json、eph-book）和新架构（Reactor、flow_steering）
- [ ] `docs/` 目录新增模块指南 — eph-json 使用指南、eph-book 订单簿指南、Reactor 多连接指南
- [ ] eph-json、eph-book 模块级 README

### 测试
- [ ] DPDK TCP 状态机行为测试 — 需要 mock 环境模拟 SYN/ACK/FIN/RST
- [ ] Reactor + flow_steering 集成测试 — 需要真实 RSS NIC（非 ENA）
