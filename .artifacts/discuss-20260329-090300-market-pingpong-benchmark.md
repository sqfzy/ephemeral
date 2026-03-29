# Discussion Record

## Context
- 时间：2026-03-29 09:03
- 耗时：~3 min
- 用户原始需求：我想同时测试接收行情和发单、收发单响应的延迟。我们之前不是实现了多连接吗？可行吗？
- 复杂度评估：中
- 讨论轮数：4 轮
- 参与角色：R14（架构师）、R3（性能狂热者）、R2（极简主义者）、R4（实用主义者）、R1（风险卫士）

## 内容摘要
议题是如何 benchmark 同时收行情和发单的延迟。代码库已有 `bench_market_pingpong_dpdk.cpp`（单连接，DPDK）和 Gateway/Reactor 多连接基础设施。核心争议是单连接 vs 双连接方案。R1 指出 Transport 的 TX/RX 走独立 TLS 路径不存在锁竞争，消除了双连接的关键动机。全员收敛到分阶段方案：先新写 kernel 版单连接 benchmark（Phase 1），验证数据后再考虑双连接增强（Phase 2）。

---

## 最终方案

### 核心决策
新写 `bench_market_pingpong.cpp`（kernel 版），与现有 `bench_market_pingpong_dpdk.cpp` 对称，单连接同时测行情接收 + 订单 RTT。双连接模式推迟。

### 方案细节
- Phase 1：kernel socket 版，度量口径与 DPDK 版一致（RX Pipeline, TX Pipeline, Order RTT, Feed Latency）
- Phase 2（可选）：双连接模式，行情和交易独立 Transport + Reactor

### 已解决的分歧
- TLS 锁竞争不存在（TX/RX 独立路径）→ 消除双连接的关键动机
- Gateway 对 benchmark 无价值 → 不使用
- Mock server 支持 echo back → 订单模拟可行

### 未解决的权衡
- 双连接实现形式：独立 binary vs 运行时 flag → 推迟到 Phase 1 数据出来再决定
