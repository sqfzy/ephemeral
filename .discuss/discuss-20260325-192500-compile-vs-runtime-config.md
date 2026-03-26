# Discussion Record

## Context
- 时间：2026-03-25 19:25:00
- 耗时：~3 分
- 用户原始需求：为什么有些设置是编译期的，有些不是？如何设计才最合理？
- 复杂度评估：中
- 讨论轮数：4 轮（第 4 轮确认收敛）
- 参与角色：R3 性能狂热者, R5 第一性原理者, R6 维护性倡导者, R7 用户代言人, R14 架构师

## 内容摘要

核心争议在于"编译期 vs 运行期配置的划线标准"。R3 主张尽可能编译期以消除热路径开销，R6 反对模板参数膨胀带来的维护成本。R5 提出三层分类（类型层/策略层/配置层），获得全员认可。最终共识：影响类型布局或代码路径存在性的设置走编译期（模板参数/宏），其余走运行期 TransportConfig。`rx_latest_only` 不应作为独立运行期设置——它是 EvictingQueue 选择的固有语义。现阶段不引入 Policy traits，等编译期开关积累到 4-5 个再重构。

---

## 分类原则（最终结论）

| 层级 | 判据 | 机制 | 当前示例 |
|------|------|------|---------|
| **类型层** | 改变 sizeof / ABI | 模板参数 | `MaxPayload`, `QueueDepth`, `RxQueueTmpl`, `TcpImpl`, `Framer` |
| **策略层** | 消除/引入整个代码路径 | 模板参数或宏 + `if constexpr` | `kEnableTimestamps`, `kIsWebSocket`, `kRxEvicting` |
| **配置层** | 同一路径内的参数调整 | 运行期 `TransportConfig` | `skip_utf8_validation`, `burst_size`, `ping_interval`, `use_tls` |

### 编译期的充要条件（满足任一）
1. 影响 `sizeof` 或内存布局
2. 控制整个代码路径的存在性（`if constexpr` 分支的判据）
3. 是另一个编译期决策的逻辑派生

### 运行期的充要条件（满足任一）
1. 值在部署时才确定
2. 值可能因实例而异
3. 不满足上述编译期的任何条件

## 关键决策

- `rx_latest_only` 不需要独立运行期设置 — EvictingQueue 语义已表达
- 现阶段不引入 Policy traits — 编译期开关数量不足以 justify
- `skip_utf8_validation` 保持运行期 — 不消除代码路径
- `use_tls` 保持运行期 — 编译期改造成本过高，未来可评估

## 完整讨论过程

（见上方对话中的 4 轮讨论）
