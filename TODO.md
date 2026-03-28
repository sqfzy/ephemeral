# TODO

## 大功能

- [x] **eph-gateway（连接管理层）** — Gateway: 多 Transport 生命周期管理 + 健康监控 + 优先级 + 20 tests。
- [ ] **UDP multicast receiver** — equity 行情接收（CME MDP3.0、Nasdaq TotalView via MoldUDP64）。`MulticastReceiver<DpdkImpl>` + IGMP join。中复杂度，2-3h。
- [ ] **io_uring 后端** — 填补 socket/DPDK 中间层。极高复杂度，需 kernel API + fallback。**优先级低，暂不实施**。

## 中功能

- [x] **MetricsSink concept** — MetricsSink concept + NullSink + ConsoleSink + 18 tests。
- [x] **eph-audit 审计日志骨架** — AuditLog: ring buffer 审计日志 + 14 event types + 文件持久化 + 14 tests。
- [x] **Crash handler + kill switch** — KillSwitch: 协调多 Transport 紧急关闭 + 信号处理 + 15 tests。

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
