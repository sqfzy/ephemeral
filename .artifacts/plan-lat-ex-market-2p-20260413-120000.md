# Plan: lat_ex_market_2p — 两阶段多币种行情延迟 benchmark

> 多币种 WebSocket bookTicker push benchmark，对比 naive（逐帧 full JSON parse）vs two-phase（轻量索引 + 选择性解析）的端到端延迟差异。

创建时间：2026-04-13
状态：已确认

---

## 定位与边界

**目标**：量化 two-phase 处理在 DPDK burst 场景下对多币种行情延迟的改善。

**用户**：eph 库开发者，评估多币种订阅场景的处理策略。

**In scope**：
- 新 benchmark binary `lat_ex_market_2p`（kernel + DPDK 双 target）
- 新 Python mock `ex_market_2p_push.py`（多币种 burst 推送）
- `bench.conf` 新 section `[lat_ex_market_2p]`
- `lat` 脚本新增 scenario 映射
- xmake.lua 为该 target 增加 `eph-json` 依赖

**Out of scope**：
- 修改现有 `lat_ex_market` 或其 mock
- SIMD 加速的 symbol scan（未来优化）
- 真连 Binance（这是 benchmark，用 mock）

---

## 架构设计

### 数据流

```
                        ┌─────────────────────────────────────────┐
                        │  ex_market_2p_push.py  (Python mock)    │
                        │                                         │
                        │  每 tick (1/push_rate_hz):              │
                        │    生成 burst_size 帧                    │
                        │    symbol = random.choice(symbols)      │
                        │    T = monotonic_raw_ns() per-frame     │
                        │    一次 sendall (all frames concatenated)│
                        └──────────────┬──────────────────────────┘
                                       │ TCP / WS binary frames
                                       ▼
┌──────────────────────────────────────────────────────────────────┐
│  lat_ex_market_2p.cpp                                            │
│                                                                  │
│  mode=naive:                                                     │
│  ┌─ on_message ──────────────────────────────────────────┐       │
│  │  t_recv = monotonic_raw_ns()                          │       │
│  │  json = eph::json::parse(data, len)                   │       │
│  │  ticker = BinanceBookTicker::from(json)               │       │
│  │  T = json.get_int("T")                                │       │
│  │  rec.record_ns(t_recv - T)                            │       │
│  └───────────────────────────────────────────────────────┘       │
│                                                                  │
│  mode=twophase:                                                  │
│  ┌─ on_message (Phase 1) ────────────────────────────────┐       │
│  │  t_recv = monotonic_raw_ns()                          │       │
│  │  hash = symbol_hash(data, len)              ~20ns     │       │
│  │  T = scan_json_uint_field(data, len, "T")   ~20ns     │       │
│  │  slot = find_or_create_slot(hash)                     │       │
│  │  slot.t_recv = t_recv                                 │       │
│  │  slot.t_server = T                                    │       │
│  │  memcpy(slot.data, data, len)               ~10ns     │       │
│  │  slot.len = len                                       │       │
│  └───────────────────────────────────────────────────────┘       │
│  ┌─ after poll() (Phase 2) ──────────────────────────────┐       │
│  │  for each non-empty slot:                             │       │
│  │    json = eph::json::parse(slot.data, slot.len)       │       │
│  │    ticker = BinanceBookTicker::from(json)             │       │
│  │    rec.record_ns(slot.t_recv - slot.t_server)         │       │
│  │    slot.active = false                                │       │
│  └───────────────────────────────────────────────────────┘       │
└──────────────────────────────────────────────────────────────────┘
```

### 核心数据结构

```cpp
// Per-symbol slot — 固定数组，最多 kMaxSymbols=64
struct Slot {
    uint32_t hash    = 0;       // FNV-1a of symbol string
    bool     active  = false;   // 本轮 poll 是否有数据
    uint16_t len     = 0;       // frame payload length
    uint64_t t_recv  = 0;       // monotonic_raw_ns at on_message entry
    uint64_t t_server = 0;      // extracted "T" value
    uint8_t  data[512]{};       // raw JSON payload copy
};
// 64 slots × 544 bytes ≈ 34KB — fits in L1
```

**Slot 查找**：线性扫描。on_message 中：
1. 扫描 `slots[0..n_symbols)` 找 `hash` 匹配 → 覆盖
2. 无匹配 + `n_symbols < kMaxSymbols` → 新增 slot
3. 无匹配 + 已满 → drop（记 `overflow` 计数）

### 测量方式

和 `lat_ex_market` 一致：
- `t_recv = bench::monotonic_raw_ns()` 在 on_message 入口
- `t_server` 从 JSON `"T"` 字段提取
- `sample = t_recv - t_server`
- warmup_samples 个 frame 丢弃
- Recorder 记录所有 post-warmup 样本

**关键差异**：naive 模式每帧都触发 full json parse（`eph::json::parse` + `BinanceBookTicker::from`），
这在 on_message 内消耗 ~200-500ns，后续帧的 `t_recv` 因此被推迟。
Two-phase Phase 1 只做 ~50ns 的轻量扫描，后续帧的 `t_recv` 更早。

**额外统计**（两种模式都输出）：
- `total_frames`：on_message 总调用次数
- `parsed_frames`：实际做了 full json parse 的帧数
- `skip_ratio`：`1 - parsed_frames / total_frames`（naive 模式为 0%）

---

## Mock 设计: `ex_market_2p_push.py`

读 `[lat_ex_market_2p]` section：

```ini
[lat_ex_market_2p]
port             = 20007
ws_path          = /ws/bookticker
push_rate_hz     = 10000
duration_seconds = 300
burst_size       = 10
symbols          = BTCUSDT,ETHUSDT,SOLUSDT,BNBUSDT,XRPUSDT
```

每个 tick（1/push_rate_hz 秒）：
1. 生成 `burst_size` 个 JSON frame
2. 每帧 symbol 从 `symbols` 列表随机选取（`random.choice`，允许重复）
3. 每帧独立 stamp `T = monotonic_raw_ns()`（在 frame 构造时）
4. 所有帧编码为 WS binary frame，拼接到一个 buffer
5. 一次 `sendall` 发出

Frame 模板（和现有 mock 一致，只 symbol 变化）：
```json
{"e":"bookTicker","s":"ETHUSDT","b":"3200","B":"5.0","a":"3201","A":"4.0","T":1712345678901234567}
```

价格字段为固定常量（benchmark 只测延迟，不测价格语义）。

---

## 需要修改的现有文件

### 1. `benchmarks/latency/xmake.lua`

在 glob loop 中为 `lat_ex_market_2p` 增加 `eph-json` 依赖：

```lua
-- 在 glob loop 内，target 定义之后：
if name == "lat_ex_market_2p" or name == "lat_ex_market_2p_dpdk" then
    add_deps("eph-json")
end
```

或更通用：对所有 target 无条件加 `eph-json`（header-only，零 link 开销）。
**选择**：特定 target 条件加，保持其他 scenario 不变。

### 2. `benchmarks/latency/bench.conf`

追加新 section：

```ini
[lat_ex_market_2p]
port             = 20007
ws_path          = /ws/bookticker
push_rate_hz     = 10000
duration_seconds = 300
burst_size       = 10
symbols          = BTCUSDT,ETHUSDT,SOLUSDT,BNBUSDT,XRPUSDT
mode             = twophase
```

### 3. `benchmarks/latency/lat`

在 `SCENARIO_MOCKS` dict 中增加一行：

```bash
[ex_market_2p]=ex_market_2p_push.py
```

---

## 实施计划

### 阶段 1: Mock server

**交付物**：`benchmarks/latency/mocks/ex_market_2p_push.py`

**内容**：
- 读 `[lat_ex_market_2p]` 的 port / push_rate_hz / duration_seconds / burst_size / symbols
- burst 推送逻辑：每 tick 生成 burst_size 帧，random.choice symbol，per-frame T stamp
- 一次 sendall 发送所有帧
- 复用现有 `_ws.py`, `_clock.py`, `_conf.py`, `_rate.py`

**验收标准**：
- `python3 mocks/ex_market_2p_push.py --config bench.conf` 启动后用 `websocat` 连接能看到多币种 JSON 帧
- 帧率大致匹配 `push_rate_hz × burst_size`
- 每个帧包含合法 `"T"` 时间戳

### 阶段 2: C++ benchmark client

**交付物**：`benchmarks/latency/exchange/lat_ex_market_2p.cpp`

**内容**：
- 读 `[lat_ex_market_2p]` 的 port / ws_path / push_rate_hz / duration_seconds / mode + globals
- `#if defined(EPH_USE_DPDK)` 切换后端（和 lat_ex_market 完全一致的模式）
- Naive 模式：on_message 中 `eph::json::parse` + `BinanceBookTicker::from` + record sample
- Two-phase 模式：on_message Phase 1 (symbol_hash + scan T + slot copy)，poll 后 Phase 2 (per-slot full parse + record)
- `--mode=naive|twophase` CLI override（覆盖 bench.conf 的 mode 值）
- 输出格式和 lat_ex_market 一致（`bench::print_report`），额外打印 total/parsed/skip_ratio
- JSON export 到 `benchmarks/latency/outputs/`

**验收标准**：
- `xmake build lat_ex_market_2p` 和 `xmake build lat_ex_market_2p_dpdk` 编译通过
- 两种 mode 都能跑完并输出合理的延迟数据
- Two-phase 的 `parsed_frames` 显著少于 `total_frames`（skip_ratio > 0）

### 阶段 3: 集成

**交付物**：修改 `xmake.lua`, `bench.conf`, `lat`

**内容**：
- xmake.lua：为 lat_ex_market_2p 目标条件添加 `eph-json` 依赖
- bench.conf：追加 `[lat_ex_market_2p]` section
- lat：SCENARIO_MOCKS 增加 `ex_market_2p` 映射

**验收标准**：
- `sudo ./benchmarks/latency/lat ex_market_2p` 端到端运行通过
- `sudo ./benchmarks/latency/lat ex_market_2p --dpdk` 运行通过（有 DPDK 环境时）
- 输出包含 latency 统计 + skip_ratio

---

## 关键决策记录

### D-1: 单 binary 双模式 vs 两个 binary
- **问题**：naive 和 twophase 怎么组织
- **决策**：单 binary + `mode` config/CLI 切换
- **理由**：同 mock、同数据、同 binary，最公平的 A/B

### D-2: eph-json 依赖
- **问题**：bench 路径是否引入 eph-json
- **决策**：引入。naive 模式需要 full json parse，two-phase Phase 2 也需要
- **理由**：这正是 benchmark 要测的——full parse vs lightweight scan 的开销差

### D-3: Mock burst 模式
- **问题**：多币种帧如何发送
- **决策**：每 tick 生成 burst_size 帧，symbol 随机选取，一次 sendall
- **理由**：模拟真实交易所行为（一个 TCP push 打包多币种更新），也是 two-phase 优化的最佳目标

### D-4: 延迟测量点
- **问题**：在哪里采时间戳
- **决策**：和 lat_ex_market 一致，`t_recv` 在 on_message 入口
- **理由**：保持和现有 benchmark 的可比性。naive 模式的 full parse 开销会自然推迟后续帧的 `t_recv`，体现差异
