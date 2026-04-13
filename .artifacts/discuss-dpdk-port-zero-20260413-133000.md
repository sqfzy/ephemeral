# Discussion Record

## Context
- 时间：2026-04-13 13:30
- 耗时：约 2 分钟
- 用户原始需求：dpdk的UdpSocket实现不支持监听0端口，但是为什么不支持呢？这个决定合理吗？Tcp呢？
- 复杂度评估：中
- 讨论轮数：3 轮（第 4 轮确认收敛）
- 参与角色：R2 极简主义者, R3 性能狂热者, R5 第一性原理者, R7 用户代言人, R14 架构师

## 内容摘要

DPDK 后端（UDP `udp.hpp:66-67`，TCP `tcp.hpp:81-84`）在 `validate()` 中拒绝 src_port=0 和 dst_port=0，而 kernel 后端允许 port=0（由 OS 分配临时端口）。争议焦点在于 src_port=0 是否应有 auto-assign fallback。R7（用户代言人）提出 hash-based fallback 方案，被 R5 以端口冲突风险否决、被 R14 以违反 DPDK "全显式配置"哲学否决。最终 5/5 角色共识：拒绝 port=0 是正确设计，唯一行动项是改进错误消息从 what 到 what+why。

---

## 核心结论

DPDK 拒绝 port=0 是正确的，原因：
1. DPDK 用户态协议栈无内核端口分配器，实现一个需要锁/位图，无真实需求
2. DPDK 包模板在构造时 bake 完整头部，端口必须确定
3. DPDK TCP 用 4-tuple 做流分类，源端口不确定会导致收包错乱
4. DPDK 配置哲学是"全显式"（MAC/port_id/queue_id 均无 auto-detect），src_port 做 auto-assign 会是唯一的隐式行为
5. 与 kernel 后端的差异是两个后端本质差异的正确反映，concept 契约约束运行时 API 不约束构造

## 建议改进

错误消息从纯 what 改为 what+why：
- `"src_port must not be zero"` → `"src_port must be explicit (DPDK has no ephemeral port allocator)"`
