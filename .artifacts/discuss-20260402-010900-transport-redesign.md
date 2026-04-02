# Discussion Record

## Context
- 时间：2026-04-02 01:09:00
- 耗时：~15 分钟
- 用户原始需求：不要拘束于现有架构。假如让你重新设计，最佳实践是什么？（延续前一个讨论：用户想要完全绕过 SPSC ring 使用 DPDK transport，收发数据都在本线程做）
- 复杂度评估：高
- 讨论轮数：7 轮
- 参与角色：R8（激进创新者）、R14（架构师）、R3（性能狂热者）、R6（维护性倡导者）、R5（第一性原理者）

## 内容摘要

核心争议在于 Transport 3461 行单文件模板类应如何分解。R8 提出的 pipeline 模式在 TX 侧可行但 RX 侧因 TLS/WS reassembly 状态依赖而不适用（R3 指出）。R14 提出的 Codec 方案经 R8 修正后拆分为 TlsCodec + FrameCodec 两个独立有状态编解码器。最终共识：5 层正交架构（Tcp → TlsCodec → FrameCodec → ConnectionManager → ThreadingPolicy），通过编译期 composition 组合，提供 3 个预制组合（QueuedTransport、InlinePipeline、RawInlinePipe），不是"重新设计"而是"重新分层"。

---

## 最终方案

### 核心决策
将 Transport 分解为 5 个正交层，通过编译期 composition 组合，支持 Inline/Queued/Reactor 三种线程模型。

### 5 层架构

```
Layer 0: TcpTransport (concept)      — 原子 TCP 操作（已有）
Layer 1: TlsCodec                    — 加解密 + record reassembly（~300 行）
Layer 2: FrameCodec<Framer>          — 帧编解码 + fragmentation + auto-pong（~400 行）
Layer 3: ConnectionManager           — 握手、重连、心跳（~500 行）
Layer 4: ThreadingPolicy             — Inline / Queued / Reactor（50-400 行）
```

### 预制组合

```cpp
// 完整版（等价当前 Transport）
using FullTransport = QueuedTransport<ConnectionManager<FrameCodec<WsFramer>, TlsCodec, Tcp>>;

// 低延迟 inline 版
using InlinePipeline = InlineTransport<FrameCodec<WsFramer>, TlsCodec, Tcp>;

// 纯 TCP + TLS（无 WS）
using RawInlinePipe = InlineTransport<TlsCodec, Tcp>;
```

### TX/RX 设计差异

- **TX 侧**：线性 pipeline（send → frame.encode → tls.encrypt → tcp.send），支持 batch coalescing
- **RX 侧**：嵌套回调 decoder（tcp.poll → tls.feed(on_record) → frame.feed(on_message)），各层管自己的 reassembly state

### 关键设计决策
1. TlsCodec 和 FrameCodec 是两个独立类（不合并）
2. 握手逻辑提取为独立 free function
3. Stats/Histogram 是可选 decorator
4. MaxPayload 是 QueuedTransport 参数，不是 Codec 参数
5. 零虚函数，全模板编译期组合
6. auto-pong 内嵌到 FrameCodec（WS 合规性要求）

### 已解决的分歧
- Pipeline vs Codec → RX 用 Codec（reassembly），TX 用线性 pipeline pattern
- TLS+WS 合并 vs 拆分 → 拆分，TX 对齐在调用层自然实现
- 泛化 framework vs YAGNI → 不建 framework，提供 3 个预制组合
- 组合爆炸 → 只预制 3 个，底层组件可独立使用但不鼓励

### 未解决的权衡
1. 渐进迁移 vs 一步到位 → 建议渐进（保留 Transport，新增 InlinePipeline）
2. ConnectionManager 是否包含在 InlinePipeline → 建议不包含（auto-pong 已在 FrameCodec 层）
