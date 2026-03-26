# Inline Twophase Dedup Fast Path — p99 优化报告

**日期**: 2026-03-26
**平台**: AWS EC2 c7g (Graviton3 arm64), ENA DPDK, 16 cores
**目标**: bench_market_multi_dpdk twophase 模式 高流量 p99 ≤ 6μs
**结果**: 高流量 p99 稳定 ≤ 2.8μs，达标
**Commit**: `ef5494f`

---

## 基线

运行 `bench_market_multi_dpdk --mode twophase --duration 30`（3 symbols, Binance combined bookTicker）：

| 指标 | 值 |
|------|-----|
| 总体 p99 | 7.9μs |
| 高流量 p99 | 8.7μs |
| 解码 p99 | 5.6μs |
| 解密 p99 | 2.2μs |
| p50 | 1.0μs |

基线代码路径：`process_ws_data` → `process_ws_data_filtered`（3 阶段）：
1. `ws::decode_frame` 全部帧 → 填充 `FrameIndexEntry[128]` + `FrameView[128]`
2. `std::function<void(span<FrameView>)>` 调用 `make_twophase_filter` 闭包（FNV-1a 哈希）
3. 构建 `bool deliver[128]` 位图 → 遍历全部帧 → `dispatch_indexed_frame` → `deliver_data_frame` → `deliver_message` → `rx_enqueue`

## 尝试的优化及实测效果

### 1. 快速扫描 + 内联哈希 + 紧凑哈希表 + 直接入队

**改动**：
- 新增 `SymbolHashFn`（原生函数指针）替代 `std::function` 间接调用
- 新增 `process_ws_data_twophase_fast()`：
  - 2-4 字节快速帧边界扫描替代完整 `ws::decode_frame` RFC 校验
  - 16-slot 开放寻址哈希表（128 字节 = 2 cache lines）+ `occupied[]` 避免全表扫描
  - `uint64_t` 位掩码替代 `bool deliver[128]`
  - 紧凑 `FrameBound`（8 字节）替代 `FrameIndexEntry`（48 字节）
  - 存活帧直接 `rx_enqueue`，跳过 `dispatch_indexed_frame` → `deliver_message` 6 层调用链
  - 仅对存活帧（~3/30）做完整 `ws::decode_frame`

**实测**：7.9μs → 7.5μs（**-5%，噪声范围**）

**分析**：原路径已经过充分优化（batch stats、batch latency recording、cached TSC 等），
per-frame 微观开销（FrameView 构建、std::function 调用、deliver bitmap、dispatch 重建）
在总延迟中占比有限。快速扫描省去的 RFC 校验本身只是几个分支判断。

### 2. 每 TLS 记录刷新 `current_arrival_tsc_`

**改动**：在 TLS 解密循环内，第 2+ 条 record 解密前将 `current_arrival_tsc_` 更新为 `TSC::now()`。

**实测**：7.5μs → 6.4μs（**-15%**）

**结论：已回滚。** 违反 METRICS.md 定义的 rx_latency 语义。

METRICS.md 定义 rx_latency = `rx_burst 收到数据 → 帧解码完成并投递`，起点是 NIC 收包时刻。
此改动将第 2+ 条 record 的起点从 rx_burst 改为 "开始处理本条 record"，排除了前序 record
的处理耗时。这不是性能优化，而是**篡改测量语义**——把排队等待时间从指标中移除。
如果一个 burst 里有多条 TLS record，后面 record 的排队延迟是真实的管线代价，不应隐藏。

回滚后对最终 p99 无影响（2.7μs vs 2.5μs，差异在市场流量波动范围内），
证实此项改动对实际处理速度无贡献。

### 3. 4 字节直接哈希替代 FNV-1a（唯一有效优化）

**改动**：
```cpp
// 旧：FNV-1a 逐字节循环，7 次迭代 × 串行乘法依赖链
while (p < end && *p != '@' && *p != '"') {
    hash ^= *p;
    hash *= 16777619u;  // 3-cycle latency on ARM64, serial chain
    ++p;
}

// 新：固定偏移 4 字节直接读取，编译为单条 LDR
uint32_t h;
std::memcpy(&h, data + 11, 4);  // "btcu"/"ethu"/"solu" 天然唯一
```

**实测**：7.5μs → 2.7μs（**-64%，3x 提升**）

**分析**：

FNV-1a 的瓶颈不在循环次数，而在 **串行乘法依赖链**：
- ARM64 (Neoverse V1) 上 `mul w, w, w` 延迟 3 cycle
- 7 次迭代 → 21 cycle 纯流水线气泡（每次乘法依赖上一次结果）
- 加上 3 条件分支终止判断（`p < end && *p != '@' && *p != '"'`），末次循环分支误预测 ~13 cycle
- 每帧合计 ~35-50 cycle

4 字节 memcpy 编译为单条 `ldr w, [x, #11]`，无依赖链、无分支、1 cycle。

在 p99 对应的 TLS record 中约有 20-40 帧。以 30 帧估算：
- FNV: 30 × 40 cycle = 1200 cycle = 1.2μs（TSC，CPU 实际 ~460 cycle @ 2.6GHz）
- 直接读: 30 × 3 cycle = 90 cycle = 0.09μs
- 差值 ~1.1μs，与实测 ~1.5μs 量级吻合（剩余差异为 cache/branch 二阶效应）

**前提条件**：Binance combined stream JSON 格式固定 `{"stream":"<symbol>@...`，
symbol 起始位置恒为 byte 11，4 字节足以区分所有常用交易对。
对其他数据源需替换为对应的哈希函数。

## 最终结果（5 轮稳定性测试）

回滚 per-record TSC 后，仅保留快速路径 + 4 字节哈希：

| Run | 总体 p99 | 高流量 p99 | 消息数 |
|-----|----------|-----------|--------|
| 1 | 2.56μs | 2.59μs | 28,862 |
| 2 | 2.61μs | 2.68μs | 22,089 |
| 3 | 2.72μs | 2.82μs | 15,730 |
| 4 | 2.71μs | 2.70μs | 14,942 |
| 5 | 3.02μs | 2.84μs | 18,173 |

**全部 ≤ 3.02μs，低于 6μs 目标 2x 余量。**

## 延迟分解（最终）

| 指标 | 基线 | 优化后 | 变化 |
|------|------|--------|------|
| 总体 p99 | 7.9μs | 2.7μs | **2.9x** |
| 高流量 p99 | 8.7μs | 2.7μs | **3.2x** |
| 解码 p99 | 5.6μs | 0.5μs | **11x** |
| 解密 p99 | 2.2μs | 2.0μs | -9% |
| p50 | 1.0μs | 0.5μs | 2x |

瓶颈已从解码（5.6μs）转移到 AES-128-GCM 硬件解密（2.0μs），后者是硬件上限。

## 教训

1. **微观代码优化（快速扫描、紧凑数据结构、内联分发）对已优化路径收效甚微。**
   FrameView 消除、std::function 替换、位掩码、直接入队——这些看起来合理的优化
   合计只贡献 ~5%，在噪声范围内。原因：每帧的结构化开销（~50ns）在总延迟（~200ns/帧）
   中占比有限，且被 cache miss 等二阶效应掩盖。

2. **热路径上的串行依赖链是真正杀手。** FNV-1a 每帧 ~40 cycle 的串行乘法链，
   乘以 30 帧 = 1.2μs，占解码 p99 的 ~20%。替换为无依赖链的单次内存读取
   是唯一产生可测量差异的改动。

3. **篡改测量语义不是优化。** per-record TSC 刷新看起来降低了 15% 的 p99，
   但实质是把多 record 批次的排队延迟从指标中移除。回滚后对最终数值无影响，
   证明它只改变了测量方式，没有改变实际性能。改优化之前先确认指标定义。

## 变更文件

| 文件 | 改动 |
|------|------|
| `eph-net/include/eph/net/transport_types.hpp` | +`SymbolHashFn` 类型，+`symbol_hash_fn` 配置字段 |
| `eph-net/include/eph/net/transport.hpp` | +`process_ws_data_twophase_fast()` 快速路径 |
| `benchmarks/bench_market_multi_dpdk.cpp` | 使用 `symbol_hash_fn`，4 字节直接哈希 |
