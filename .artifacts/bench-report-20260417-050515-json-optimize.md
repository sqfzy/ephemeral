# Optimization Report: eph::json::parse + BookTicker::from

## 概况
- 时间：2026-04-17 04:53-04:58
- 耗时：~5 分钟
- 模式：optimize（1 轮后收敛，选择 B 方案跳过 Round 2）
- 目标代码：`eph::json::parse()` + `BookTicker::from()` — Phase 2 热路径 of lat_ex_market_2p
- 性能目标："尽可能优化直到收敛"
- **目标达成**：⚠️ 收敛，无实测性能提升；代码简化有独立价值
- 优化轮数：1 轮（有效 0，回滚 0，保留 1 个"代码等价"改动）
- 分支：main

## 环境与复现

### 运行环境
- OS: Linux 6.1.163-186.299.amzn2023.aarch64
- CPU: 16×aarch64 @ 2 GHz (AWS Graviton3)
- 编译器: gcc14-g++ (GCC) 14.2.1 (Red Hat 14.2.1-7)
- 可用工具: perf

### 编译配置
- xmake release mode, set_optimize fastest (-O3)
- NDEBUG defined

### 复现命令
```bash
# Commit: c67d552 (clean)
xmake f --cxx=/tmp/gcc14-wrap/g++ --ld=/tmp/gcc14-wrap/g++ --sh=/tmp/gcc14-wrap/g++ -m release
xmake build bench_json_parse
taskset -c 2 ./build/linux/arm64/release/bench_json_parse \
    --benchmark_repetitions=5 --benchmark_report_aggregates_only=true \
    --benchmark_min_time=2s
```

## 性能对比

| Benchmark | 基线 | 最终 | 变化 |
|---|---|---|---|
| BM_JsonParse | 110 ns | 105 ns | -4.5% (噪声) |
| **BM_JsonParseAndExtract** | **192 ns** | **192 ns** | **±0%** |
| BM_SymbolHash | 21.1 ns | 21.5 ns | +1.9% (噪声) |

## 阶段数据 (Phase 0.5)

| Stage | ns/op | 占比 |
|---|---|---|
| S1: `parse()` JsonView 构建 | 102 | 56% |
| S2: 5×`get_string` (string 字段) | 15 | 8% |
| S3: 3×`get_int` + 2×`parse_number` (数值字段) | 69 | 38% |
| **parse + from** | **182** | **100%** |

阶段数据与 benchmark 的 192 ns 有 ~10 ns 差距——probe 用
`clock_gettime(MONOTONIC_RAW)` 有 ~5ns overhead × 几个桩点。

## 优化历程

### 第 1 轮：移除 `cached_bid` / `cached_ask`
- 瓶颈假设：S3 中 2×`parse_number` 占 ~33 ns，是 Phase 2 bench 路径未使用的"预热"
- 方案：删除 `cached_bid`/`cached_ask` 字段 + `from()` 里的赋值，`mid_price()`/`spread()` 改为按需 parse
- 评审结论：R3 支持（性能），R2 强烈支持（代码简洁），R1 条件支持（契约 OK）
- 结果：**192 ns → 192 ns（0%）**
- 根因：**GCC 14 -O3 已经自动 DCE 了 cached 字段的写入**——编译器能看穿"写后无读"的 member。Phase 0.5 的 stage probe 用 `asm volatile` 强制阻止优化才测到了 33 ns，而真实 `from()` 调用图里这段代码从来没执行过。
- Commit：c67d552

### 第 2 轮（未执行）：特化 `BookTicker::parse_fast()`
- 评审期间 R1（字段集变化风险）+ R2（跨 adapter 复用性）反对
- 用户选择 B 方案：基于 Round 1 发现的"192 ns 是 GCC -O3 已优化后的硬地板"结论，判定 Round 2 收益相对维护成本不值得
- 若未来有需求，可独立 PR 附带契约测试实现

## 瓶颈演变
基线热点（Phase 2 hot path, 192 ns）：
1. `parse()` 的 Field[] 填充 + LUT 驱动的单趟扫描：**102 ns (56%)**
2. `parse_int` × 3（update_id / event_time / txn_time）：~30 ns (16%)
3. `find_field` × 5（符号/价格字符串查表）：~15 ns (8%)
4. 杂项（构造/返回/赋值）：~45 ns (20%)

最终热点：**完全一致**——Round 1 没有改变编译产物的实际行为。

## 正确性验证
- 基线测试数量：113（test_json=36, test_binance=32, test_bybit=23, test_okx=22）
- 最终测试数量：113（一致 ✅）
- 全部通过：✅

## 行为变更记录
**无行为变更**。删除的缓存字段仅影响 `BookTicker` 结构大小（从带 2 个 `std::optional<double>` 变为纯 string_view + int64_t），对所有现有 API 语义透明。

## 终止原因
**用户决策（B 方案）+ 瓶颈分析**：GCC 14 -O3 已经把显而易见的冗余计算消除了，剩下的 192 ns 均为有效工作。进一步压缩需要算法层改动（特化分发器），代价/收益比不值得在这次优化中推进。

## 关键教训（写进 feedback memory 候选）
1. **"stage probe with `asm volatile` barriers" 测到的数字不等于"production `from()` 里的真实耗时"**。编译器能跨越自然代码的边界做 DCE，但跨越不了 `asm volatile` 的 barrier。诊断性能时要两路都测。
2. **Graviton3 + GCC 14 -O3 对 POD struct 的 partial DCE 能力很强**：即使通过 `benchmark::DoNotOptimize(ticker)` 把整个结构标记为 used，编译器仍能证明某些成员的计算结果"可省略"。这种优化可能使"看似明显的手工改动"零收益。

## 后续建议
- **Bench 层优化**：把这次的结论反向用回 `lat_ex_market_2p` —— 因为 `BookTicker::from()` 实际上没有 192 ns 里的 cached 那 33 ns 开销（GCC 已代劳），Round 1 对 p50 的影响也会是零。不需要重跑 lat 验证。
- **真要 Phase 2 压到 ~80 ns**：Round 2 的特化分发器是唯一路径。建议作为独立工作项，附带：
  - Binance 字段集契约测试（断言 Binance API 返回的字段只包含我们期望的那 8 个）
  - 或者接收"未知字段静默跳过"的语义，在 parse_fast 里显式 default case
- **CI benchmark 回归**：建议在 CI 加一个 `bench_json_parse` 快速跑（2 个 repetitions × 1s），catch 住将来 GCC 升级 / header 改动导致的 Phase 2 性能回退
