# Discussion Record

## Context
- 时间：2026-04-18 14:30:00
- 耗时：约 28 分钟
- 用户原始需求：当前 MetricsSink concept 是否是网络模块观测体系的最佳承载方案？需要评估其设计权衡，并给出几个可行的备选方案。
- 复杂度评估：高
- 讨论轮数：4 轮
- 参与角色：R3 性能狂热者、R6 维护性倡导者、R8 激进创新者、R12 系统思维者、R11 怀疑论者（均为预定义角色）

## 内容摘要

讨论围绕"如何在 HFT 网络模块中接入观测体系"展开。核心争议为 push model（MetricsSink 接口注入）vs pull model（内置 atomic counter + 外部 snapshot），以及是否保留现有的 `eph::core::MetricsSink` concept。R3 与 R6 最初分别主张模板化与运行时多态，R8 提出彻底抛弃 push 改用内置 atomic counter，R12 警告不可碎片化、必须考虑 OTel 兼容，R11 全程要求实证。R11 在第 3 轮揭示项目已有 `Recorder`/pull 范式后，方案重心向 R8 倾斜。R12 提出"两层架构"调和——底层 enum-indexed atomic counter（hot path），上层 MetricsSink 做应用层适配 push——获得 R3/R6/R8 三方接受。最终共识为该两层架构，分四阶段实施，前置验证 alignas atomic 在 -O2 下生成的指令序列。

---

## 复杂度评估

**复杂度：高 → 讨论轮数：4 轮（最低）**

理由：
- 多个合理方案，存在非显然权衡（性能/可维护性/灵活性）
- 跨模块影响（4 个 stream 类 + 上层应用）
- 涉及项目长期演进方向（HFT 观测体系不止 metrics，还有 tracing/logging）

## 上下文（已读代码）

- `eph-core/include/eph/core/metrics_concept.hpp` — MetricsSink concept + NullSink
- `eph-utils/include/eph/utils/console_sink.hpp` — ConsoleSink 参考实现
- `tests/integration/test_metrics_concept.cpp` — concept 测试 + RecordingSink 示例
- `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp` — 网络模块本体
- `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp` — 同
- `eph-utils/include/eph/utils/recorder.hpp` — 项目既有 pull model 工具

关键事实：网络模块 grep `MetricsSink` / `push_counter` 均无匹配 → 现有 MetricsSink 是**预留的设计空位**，从未被任何 production 模块使用。

---

## 第 1 轮 — 各角色独立提出核心论点

### R3 性能狂热者 | 部分支持现有 MetricsSink

论点：现有 `MetricsSink` concept + `NullSink` + `[[no_unique_address]]` 模式
在编译期可被完全消除——这是 HFT 唯一可接受的形态。但当前 concept 设计有
两个问题：
1. `std::span<const MetricTag> tags` 参数构造可能在栈上落实物
2. tag value 动态生命周期管理在 hot path 不可行

方案：保留 MetricsSink concept 但加约束：
- 所有 tag value 必须是 `constexpr std::string_view`
- 引入 `push_counter_fast(name, value)` 无 tag 重载
- 用 `[[no_unique_address]] M metrics_` 嵌入 stream 类

### R6 维护性倡导者 | 反对单纯模板化

论点：`KernelTcpStream<C, EnableTls, M>` 加第三个模板参数代价：
1. IDE hover 信息变难读
2. 多种 sink → instantiation 数量乘积膨胀
3. 应用层无法运行时切换 sink
4. 所有别名要么默认 NullSink（无观测）要么全改

方案：用 IMetricsSink 接口对象 + cold path virtual call。
hot path 加 `if (metrics_) [[unlikely]]` 检查。

### R8 激进创新者 | 反对推送模型

论点：MetricsSink 是 push model——hot path 主动调 sink。HFT 不该这样。
问题：
- NullSink no-op 但分支预测仍占空间
- 观测点散落污染热路径
- 改 metric 名要重新编译部署

方案：pull model + 内置原子计数器
- stream 类持有 `std::atomic<uint64_t>` 具名成员
- hot path 单次 atomic ++（relaxed）
- 外部 `snapshot()` 接口读取
- 应用层完全控制 push 频率

### R12 系统思维者 | 反对孤立看 metrics

论点：完整观测体系 = metrics + tracing + logging。现有 MetricsSink 只覆盖 metrics。
应该考虑：
- 是否要统一 ObservabilitySink？
- tracing context 怎么传递？
- 行业标准 OpenTelemetry 兼容性

方案：扩展 MetricsSink 加 trace span / structured event 接口。

### R11 怀疑论者 | 全面质疑

论点：所有方案都缺数据支撑。必须验证：
1. NullSink 真的零开销？需要 objdump 验证
2. HFT 真的需要每帧推 metrics？还是采样/事件触发？
3. MetricsSink concept 本身从未在网络模块用过 — 是未验证的理论设计

立场：拒绝在没有数据的情况下选择任何方案。

---

## 第 2 轮 — 直接反驳，临时联盟

### R3 性能狂热者 | 反对 R6 的 virtual 方案

反驳 R6：HFT bench 里 `on_message` 10M ops/s。virtual call ~5ns × 10M = 5% CPU。
nullable pointer 模式还导致 production != staging 测试覆盖。

反驳 R8：atomic counter 单线程开销可接受，但 false sharing：10 个 counter 同
cache line → reader 抢锁。

反驳 R12：观测体系完整性对，但现在不是引入 ObservabilitySink 的时机，scope creep。

联盟 R11：同意"先 micro-bench 再决定"作为前置条件。

### R6 维护性倡导者 | 部分让步

反驳 R3：5% CPU 估算假设每帧调用——这正是 R8 反对的 push model。批量/采样
推送 → virtual 开销可忽略。

反驳 R8：atomic counter snapshot 接口牺牲灵活性：
- 用户加业务 metric 不能注入
- 多 stream 实例区分困难
- 采样策略无法做

让步给 R3：模板化是对的，但需要保留逃生通道（RuntimeMetricsAdapter 兼容
NullSink 但内部持 IMetricsSink*）。

### R8 激进创新者 | 反击 push model 联盟

反驳 R3 false sharing：每个 counter `alignas(64)` 即可，多花 640 字节。

反驳 R6 灵活性：业务 metric 业务层自己有；多 stream reader 端附加 tag；
采样 reader 端控制频率——都不需要 sink 接口。

关键论点：pull model 让 stream 类只负责"如实记录"，不负责"如何上报"。
单一职责原则。

反驳 R12：tracing/logging 独立设计，spdlog 已是 logging 事实标准；tracing 做
独立 TracingContext，与 metrics 正交。

### R12 系统思维者 | 调和但坚持原则

让步给 R3：现在不强推 ObservabilitySink。

反驳 R8：硬编码具名 metric 导致：
- 加新 metric = 改 header = 全项目 recompile
- 不同应用要求不同 metric set 拉扯 stream 类
- 接入 OTel 时迁移成本爆炸

反驳 R6 RuntimeMetricsAdapter：双轨制是运维灾难。

联盟 R3：支持模板化但建议命名规范（OTel 风格）。

### R11 怀疑论者 | 推动实证

要求验证：
1. NullSink -O2 是否生成 0 指令（objdump）
2. virtual call 实际开销（nanobench）
3. atomic relaxed fetch_add 实际开销（同上）

要求澄清需求：
1. metric 实际更新频率
2. 应用层消费节奏
3. 是否需要运行时切换 sink
4. 多线程访问 stream 内部 counter 的并发模型

反驳 R3 LMAX/Aeron 类比：诉诸权威而不给链接，Aeron Counter 实际是 shared
memory + atomic 模式。

立场：拿到三个 nanobench + 四个需求澄清前不可决策。

---

## 第 3 轮 — 聚焦核心分歧

### R11 怀疑论者 | 提供事实 + 澄清需求

事实：
1. NullSink 零开销假设合理但仍需 micro-bench 实证
2. **项目内已有 pull model 范式**：`eph-utils/include/eph/utils/recorder.hpp` 的
   `Recorder` / `ConcurrentRecorder` 就是 pull 模式
3. MetricsSink concept 是预留设计空位，无 production caller

澄清需求：
- metric 频率分两类：高频（每帧 bytes_recv 等） vs 事件触发（cross_record 等）
- reader 节奏：典型 100ms-1s
- 运行时切换 sink：HFT 通常不需要
- 多线程：每 stream 单线程持有，reader 从外部 snapshot

→ R8 的 pull model 与项目既有 Recorder 模式更一致

### R8 激进创新者 | 受 R11 启发深化方案

承认 R12 的"硬编码 metric 名"问题，提出 **enum-indexed atomic counter array**：

```cpp
enum class StreamMetric : size_t { kBytesSent, kBytesRecv, ..., kCount };

class KernelTcpStream {
    struct alignas(64) Counter { std::atomic<uint64_t> v{0}; };
    std::array<Counter, size_t(StreamMetric::kCount)> counters_;

    template <StreamMetric M>
    void inc(uint64_t n = 1) noexcept {
        counters_[size_t(M)].v.fetch_add(n, std::memory_order_relaxed);
    }

    uint64_t get(StreamMetric m) const noexcept {
        return counters_[size_t(m)].v.load(std::memory_order_relaxed);
    }
};
```

优点：加新 metric 只改 enum；reader 泛型 snapshot；编译期分派；alignas
防 false sharing。

### R3 性能狂热者 | 接受 R8 改进方案

立场修正：从模板化 MetricsSink 转向支持 R8 atomic 方案。
理由：atomic 不仅不 virtual，还不 push，根本不需要任何接口分派。承认
push model 在 HFT 高频场景下不如 pull 干净。

性能细节确认：alignas(64) + relaxed atomic = `lock add` 单指令（~5-10 ns），
比 NullSink 理论零开销差但可观测、可验证、可信赖。

### R6 维护性倡导者 | 接受 R8 + 提出补充

立场修正：放弃双轨制，支持 R8 方案。
理由：R8 方案没有模板膨胀，ABI 稳定，IDE 体验好。

补充：
1. enum + name 映射表 static_assert 强制对齐
2. enum 加新成员需 review 讨论
3. 业务 metric 不进 stream counter

OTel 映射表方案：
```cpp
constexpr std::string_view kStreamMetricName[] = {
    "net.tcp.bytes_sent", ...
};
```

### R12 系统思维者 | 反对 R8 抛弃 MetricsSink

反驳 R8：
1. 失去 tag 维度——多 stream 实例区分依赖 reader 知道业务语义
2. 无法做 histogram——atomic counter 不能存分布
3. 重复造轮子——废弃 metrics_concept.hpp 全套 API

**调和方案：两层架构**
- 底层：R8 enum-indexed atomic counter（hot path 唯一接触）
- 上层：MetricsSink 适配层（reader 周期性 snapshot 后批量推 sink）
- stream 类只持底层，sink 不进 stream 模板参数

---

## 第 4 轮 — 立场修正，最终收敛

所有角色支持 R12 的两层架构方案。

### R3 立场修正
从"compile-time MetricsSink + 模板参数"修正为"R8 底层 + MetricsSink 上层适配"。
认识到 push 不进 hot path 才是关键，sink 类型是不是模板参数无所谓。

### R6 立场修正
从"双轨制 escape hatch"修正为"分层架构"。双轨制问题是两种约定，分层架构
是一种约定两种角色，职责清晰。

### R8 立场修正
从"抛弃 MetricsSink"修正为"保留 MetricsSink 作为上层适配，但 stream 内部不
依赖它"。承认 histogram 和 tag 论点站得住。

### R12 立场微调
放弃统一 ObservabilitySink 扩展。Tracing/logging 单独设计。

### R11 立场修正
从"全面否决"修正为"分阶段验证 + 实施"。仍坚持 Phase A 验证（godbolt + objdump）
通过后才能在 production 模块大范围铺开。

---

## 最终方案

### 核心决策

采用两层观测架构：底层为 stream 内嵌的 enum-indexed atomic counter array
（pull model，零热路径成本），上层保留并复用现有 `MetricsSink` concept 作为
应用层 push 适配。

### Layer 1 — Stream 内置原子计数器（hot path）

新文件 `eph-net/include/eph/net/stream_metrics.hpp`：

```cpp
namespace eph::net {

enum class StreamMetric : std::size_t {
    kBytesSent,
    kBytesRecv,
    kFramesDecoded,
    kReasmOverflows,
    kCodecErrors,
    kTlsCrossRecordFrames,    // DPDK TLS only
    kCount
};

inline constexpr std::array<std::string_view,
                            static_cast<std::size_t>(StreamMetric::kCount)>
kStreamMetricNames = {
    "net.stream.bytes_sent",
    "net.stream.bytes_recv",
    "net.stream.frames_decoded",
    "net.stream.reasm_overflows",
    "net.stream.codec_errors",
    "net.stream.tls.cross_record_frames",
};
static_assert(kStreamMetricNames.size() ==
              static_cast<std::size_t>(StreamMetric::kCount),
              "metric enum and name table out of sync");

} // namespace eph::net
```

每个 stream 类内嵌：

```cpp
struct alignas(64) Counter { std::atomic<std::uint64_t> v{0}; };

std::array<Counter, static_cast<std::size_t>(StreamMetric::kCount)> counters_{};

template <StreamMetric M>
void inc_(std::uint64_t n = 1) noexcept {
    counters_[static_cast<std::size_t>(M)].v.fetch_add(
        n, std::memory_order_relaxed);
}

[[nodiscard]] std::uint64_t metric(StreamMetric m) const noexcept {
    return counters_[static_cast<std::size_t>(m)].v.load(
        std::memory_order_relaxed);
}
```

### Layer 2 — MetricsSink 适配层（应用层选择）

新文件 `eph-net/include/eph/net/stream_metrics_collector.hpp`：

```cpp
/// @brief Read all stream counters and publish them to a MetricsSink.
///
/// Naming: `publish` matches Prometheus / OpenTelemetry conventions
/// (cf. OTel `MetricExporter`, Prometheus push gateway "publish").
/// Avoids the redundancy of `publish_metrics(..., Sink& sink)`.
template <typename Stream, ::eph::core::MetricsSink Sink>
void publish_metrics(const Stream& source, Sink& sink,
                     std::span<const ::eph::core::MetricTag> tags = {}) {
    for (std::size_t i = 0;
         i < static_cast<std::size_t>(StreamMetric::kCount); ++i) {
        sink.push_counter(kStreamMetricNames[i],
                          static_cast<std::int64_t>(
                              source.metric(static_cast<StreamMetric>(i))),
                          tags);
    }
}
```

### Layer 3（未来扩展，本次不做）

延迟分布 histogram 走 `eph::utils::Recorder`（已有），独立成员。

### 实施分阶段

- **Phase A**：godbolt + objdump 验证 alignas atomic 在 -O2 生成单条 lock add，
  enum-indexed array 编译期常量索引被消除（~1 小时）
- **Phase B**：单 stream pilot（KernelTcpStream），加 counter array + hot path
  调用 + RecordingSink 验证测试 + lat_ws bench 前后对比
- **Phase C**：铺开到 DpdkTcpStream / KernelUdpSocket / DpdkUdpSocket
- **Phase D**：写参考 reader（`publish_metrics` 示例）加到 `examples/`

### 已解决的分歧

| 分歧点 | 解决方式 | 关键论据 |
|--------|----------|----------|
| push vs pull model | pull（atomic counter + 外部 snapshot） | 与项目既有 Recorder 哲学一致；hot path 零分派；R8/R3/R6 共识 |
| 是否保留 MetricsSink | 保留作为上层适配 | tag/histogram/multi-sink 需求要求 push 接口 |
| 模板参数 vs 内嵌 vs 指针 | 内嵌定长 array | 模板膨胀不可接受 + virtual 不可接受 + 双轨制反模式 |
| metric 命名 | OTel 风格层级化（net.stream.*） | 便于未来对接 |
| 加新 metric 流程 | enum + name 映射 + static_assert | 防漂移 |
| 业务 metric 是否进 stream counter | 否，业务层独立 | 单一职责 |

### 未解决的权衡

| 冲突 | 角色 A | 角色 B | 建议 |
|------|--------|--------|------|
| 是否阻塞实施等 micro-bench | R11：必须先做 | R3+R8：理论上可信 | 折中：Phase A 最小验证（godbolt + objdump 1 小时内可完成）|
| publish_metrics 是模板还是 type-erased | R3：模板 | R6：type-erased | 建议模板（reader 端不在 hot path）|
| 多 stream snapshot 的并发模型 | （未深入） | | 留给 reader 应用决定 |

### 收敛路径总结

第 1 轮各方独立提案 → 第 2 轮 R3/R6/R8/R12 互相反驳 → 第 3 轮 R11 揭示
"项目已有 pull 范式"事实改变方向，R8 提出 enum-indexed 方案，R12 提出
"两层架构"调和 → 第 4 轮所有角色立场修正支持两层架构。

---

## 备选方案速查（用户原本要"几个可行方案"）

按本轮讨论收敛的优先级：

### 方案 1（推荐）：两层架构 = enum atomic counter + MetricsSink 适配
- hot path：零分派 + 单条 lock add
- 应用层：通过 `publish_metrics` 周期性推送给任何 MetricsSink 实现
- 改动：新增 2 个 header + 4 个 stream 类各加 ~20 行 + 1 个集成测试

### 方案 2：模板化 MetricsSink（被讨论否决）
- `KernelTcpStream<C, EnableTls, M = NullSink>` + `[[no_unique_address]] M metrics_`
- 优：编译期完全消除
- 劣：模板签名膨胀；不能运行时切换；instantiation 爆炸；hot path 仍有 push 调用
- 改动：4 个 stream 类签名扩展 + 所有别名维护

### 方案 3：IMetricsSink* runtime 注入（被讨论否决）
- nullable pointer + virtual call
- 优：运行时切换灵活
- 劣：vtable 开销污染 hot path；nullptr 检查导致 prod/staging 路径分叉
- 改动：新增 IMetricsSink 抽象类 + 4 个 stream 类持指针

### 方案 4：扩展为 ObservabilitySink（被讨论延后）
- metrics + tracing + structured event 统一接口
- 优：观测体系完整、可对接 OTel
- 劣：scope creep，当前只解决 metrics 即可
- 暂缓，可在 Layer 3+ 阶段考虑

### 方案 5：完全跳过 MetricsSink，纯内置 atomic counter
- 仅做 Layer 1，不做 Layer 2 适配
- 优：最简
- 劣：每个用户都要自己写 reader 转 sink；无 tag/histogram；与 metrics_concept.hpp
  现有设计割裂
- 不推荐——R12 的论点站得住

