# Discussion Record

## Context
- 时间：2026-04-02
- 用户原始需求：你觉得DirectTransport和DirectTxTransport这种代码设计是最佳实践吗？它们的使用接口不统一
- 复杂度评估：中
- 讨论轮数：4 轮
- 参与角色：R14 架构师, R3 性能狂热者, R2 极简主义者, R6 维护性倡导者, R4 实用主义者

## 内容摘要

三个 transport 类（Transport, DirectTxTransport, DirectTransport）的接口不一致性主要体现在：send_for() 同名不同语义（Transport 等待 vs Direct 忽略 timeout）、RX 侧 API 完全断裂（recv vs feed_rx+process_pending）、大量重复代码（send_close/send_ping）。

讨论收敛到：三类独立存在是合理的（不同性能模型需要不同 API），但存在接口卫生问题。建议分两阶段修复：Phase 1 立即修复 send_for() 语义分裂 + 提取重复代码；Phase 2 引入轻量级 concept 约束共享接口。合并三个类的方案被否决（编译复杂度 + 可读性下降）。

## 最终方案

### 核心决策
三类 transport 设计总体合理，但需修复接口卫生问题（send_for 语义分裂、代码重复）。

### Phase 1（立即执行）
1. 修复 send_for() 语义分裂：Direct 变体中删除 timeout 参数或标记 deprecated
2. 提取 send_close()/send_ping() 重复逻辑到 TransportCore helper
3. 文档明确标注每个变体的 send 语义

### Phase 2（下阶段）
1. 引入 TransportSender concept
2. 引入 TransportLifecycle concept

### 已解决的分歧
- 是否合并三个类 → 否（编译复杂度 + 可读性）
- RX 侧是否统一 → 否（不同数据流模型）

### 未解决的权衡
- concept 粒度：最小化 vs 全面覆盖 → 建议从最小开始
