# Discussion Record

## Context
- 时间：2026-03-26 08:45:00
- 耗时：~5 分钟
- 用户原始需求：mode all 和 twophase，这是应用层的事，dpdk只应该提供一个跳过的能力，由上层注入实现，不是吗？
- 复杂度评估：中
- 讨论轮数：4 轮
- 参与角色：R14 架构师、R3 性能狂热者、R2 极简主义者、R5 第一性原理者、R4 实用主义者

## 内容摘要

核心争议：symbol dedup 的 twophase 策略逻辑（~200 行）当前硬编码在 `Transport::process_ws_data_symbol_dedup` 中，Transport 直接理解 `SymbolDedup` 枚举和 `SymbolExtractorFn`。用户质疑这是否应该上提到应用层。

R14/R2/R5 认为 transport 不应包含业务层 dedup 策略，应提供批量帧过滤回调。R3 担心抽象引入性能开销，但被 R2 反驳（`symbol_extractor` 已是 `std::function` 且每帧调用，filter 每批仅一次）。R4 要求保持调用方简洁性，通过 helper 函数解决。

最终收敛：Transport 提供 `on_frame_filter(span<FrameView>)` 回调，删除内置 dedup 策略代码和 `SymbolDedup` 枚举，将 twophase 逻辑抽为 `make_twophase_filter()` 工具函数。

---

## 第 1 轮

**【R14 架构师 | 应该上提到应用层】**

论点：当前设计违反单一职责。`Transport` 的职责是可靠地传输 WS 帧，不应理解"symbol"这个业务概念。`SymbolDedup`、`SymbolExtractorFn`、`binance_symbol_hash` 全是应用层语义。正确分层：transport 提供帧过滤回调（如 `on_frame_filter`），RX 线程中回调，返回 deliver/skip。

**【R3 性能狂热者 | 反对，dedup 必须在 transport 内部】**

论点：dedup 核心收益来自批量处理整个 TLS record 的帧索引——Phase 1 扫描所有帧头，Phase 2 基于全局信息决策。需要访问 transport 内部原始解密缓冲区。回调每帧独立决策会失去全局视角。

**【R2 极简主义者 | 同意上提，比 R14 更激进】**

论点：transport 里现在有三条数据路径：普通 `process_ws_data`、`LastOnlyDeliver` 优化、`process_ws_data_symbol_dedup`。应该把后两者都抽出，transport 只做 decode + deliver。

**【R5 第一性原理者 | 重新定义问题】**

论点：真正的设计问题不是"dedup 放哪"，而是 transport 的 delivery 机制是否支持批量决策。dedup 本质是 `on_message` 的特化——如果回调能看到整批帧并决定跳过，就不需要 transport 内置 dedup。

**【R4 实用主义者 | 维持现状】**

论点：当前设计工作良好，benchmark 证明 twophase 降低 p99 40%+。重构成回调注入会把 2 行配置变成 50 行样板。受益者只有"架构审美"。

---

## 第 2 轮

**【R14 架构师】**

反驳 [R3]：transport 可提供**批量过滤回调** `on_batch_filter(span<FrameIndexEntry>, deliver_bitmap&)`，Phase 1 建完索引后调用。帧扫描是 transport 职责，但"哪些帧有价值"是业务逻辑。
反驳 [R4]：helper 函数 `make_symbol_dedup_filter(binance_symbol_hash)` 可保持 2 行配置。

**【R3 性能狂热者】**

反驳 [R14]：`on_batch_filter` 要把 `FrameIndexEntry` 暴露为公共 API——`offset`、`mask_key`、`payload` 指针全是内部实现。回调是 `std::function`，RX 热路径有虚调用开销。
联盟 [R4]。

**【R2 极简主义者】**

反驳 [R3]：不需要暴露 `FrameIndexEntry`。batch filter 只需 `span<pair<uint32_t symbol_hash, bool& deliver>>`。关于 `std::function` 开销——`symbol_extractor` 本身已是 `std::function` 且每帧调用一次，如果接受 extractor 开销，没理由拒绝每批一次的 filter。

**【R5 第一性原理者】**

新方案：transport 提供 `symbol_extractor`（已有），再提供 `on_batch_deliver: fn(symbols: &[u32]) -> BitVec`。twophase 逻辑变成 ~20 行 lambda，由 eph-net 提供为 utility。

**【R4 实用主义者】**

让步条件：如果能做到旧 API 兼容或 helper 函数保持调用方简洁性，同意重构。

---

## 第 3 轮

**【R14 架构师】** 收敛方案：`on_frame_filter(span<FrameEntry>)` 其中 `FrameEntry = { uint32_t symbol_hash; bool deliver }`。删除 `SymbolDedup` 枚举。提供 `filters::twophase_latest()` 工具函数。
联盟 [R2, R5]。
反驳 [R4]：`kTwoPhaseLatest` 3 天前才加，无外部用户，无兼容负担。

**【R3 性能狂热者】** 有条件同意：`on_frame_filter` 应为模板参数而非 `std::function`；`FrameEntry` 必须 trivial，`span` 指向栈上。

**【R2 极简主义者】** 反驳 [R3] 模板化：`symbol_extractor` 已是 `std::function` 每帧调用，filter 每批一次，开销更小。模板化会给 `Transport`（已 6 个模板参数）增加编译成本。

**【R5 第一性原理者】** 补充：`symbol_extractor` 也应可选。让 filter 直接看到 payload，自己决定算不算 hash。`FrameView = { payload, len, opcode, bool& deliver }`，`symbol_extractor` 变成 twophase filter 内部细节。

**【R4 实用主义者】** 同意，条件：提供 `make_twophase_filter(extractor)` helper，调用方两行搞定。

---

## 第 4 轮（收敛）

所有角色达成一致：

**【R14】** 最终方案：`FrameView { payload, len, opcode, fin, deliver=true }`，`FrameFilterFn = function<void(span<FrameView>)>`，替代 `symbol_dedup + symbol_extractor`。

**【R3】** 接受 `std::function`（R2 论点有效）。要求 `FrameView` 是 POD、`span` 指向栈。

**【R2】** 满意：transport 删 ~200 行策略代码，`SymbolDedup` 枚举删除。

**【R5】** 补充 `FrameView` 加 `bool fin` 让 filter 安全跳过分片帧。

**【R4】** 同意，只要有 `make_twophase_filter(ExtractorFn)` helper。

---

## 最终方案

### 核心决策
Transport 不应内置 dedup 策略逻辑；提供批量帧过滤回调 `on_frame_filter`，让应用层注入 deliver/skip 决策。

### 方案细节

1. **新增公共类型**：
```cpp
struct FrameView {
    const uint8_t* payload;
    uint16_t len;
    uint8_t opcode;
    bool fin;
    bool deliver = true;
};
using FrameFilterFn = std::function<void(std::span<FrameView>)>;
```

2. **TransportConfig 变更**：
   - 删除：`SymbolDedup symbol_dedup`、`SymbolExtractorFn symbol_extractor`
   - 新增：`FrameFilterFn on_frame_filter{}`

3. **Transport 内部**：
   - 删除 `process_ws_data_symbol_dedup` 全部 ~200 行
   - 删除 `SymbolDedup` 枚举
   - 帧索引扫描后若 `on_frame_filter` 有值，构建 `FrameView[]`（栈上），调用 filter，根据 `deliver` 位分发

4. **工具函数**：
```cpp
// eph/net/filters.hpp 或 transport_types.hpp
FrameFilterFn make_twophase_filter(std::function<uint32_t(const uint8_t*, size_t)> extractor);
```

5. **调用方代码**：
```cpp
tc.on_frame_filter = eph::net::make_twophase_filter(binance_symbol_hash);
```

### 已解决的分歧
- "transport 需要全局帧视角" → `span<FrameView>` 批量回调
- "`std::function` 性能" → 每批一次，开销 <50ns
- "调用方复杂度" → `make_twophase_filter()` helper

### 未解决（需用户决策）
- 是否保留 `symbol_extractor` 在 config 中（供多种 filter 复用），还是完全下沉为 filter 内部参数
