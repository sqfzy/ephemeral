# Plan: Phase 12 — eph-net-dpdk 性能调优（回 baseline parity + 修一个 silent-drop bug）

> 修 v3.3 集成时引入的 1 个 Critical bug 和 3 个性能回退，让 `DpdkPoller` 在单类型工作负载下回到
> baseline Reactor 等价的热路径，不触碰 std::function API（后者收益有限、用户体验冲击大）。

创建时间：2026-04-11
状态：已确认

参考：
- `.artifacts/review-eph-net-dpdk-perf-20260411-130628.md` — 审查报告
- `.temp/baseline-pre-v3.3/eph-dpdk/include/eph/dpdk/reactor.hpp` — baseline 参考实现
- `eph-net-dpdk/include/eph/net/dpdk/poller.hpp` — 待修复
- `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp` — ReasmBuffer 位置

---

## 定位与边界

**目标**：把 v3.3 DpdkPoller + DpdkTcpStream 的热路径性能回到 pre-v3.3 baseline Reactor 等价水平，同时修掉 v3.3 集成时引入的 ReasmBuffer silent-drop bug。

**用户**：HFT tick-trader 在 DPDK 路径上运行 lat_ex_md_udp / lat_ex_market 等高 pps 场景，对每包 5-20 ns 敏感；以及任何碰上 reasm buffer 溢出场景的用户（silent drop 是可靠性问题）。

**In scope**:
- `eph-net-dpdk/include/eph/net/dpdk/poller.hpp`：
  - `PollableEntry` 结构重新布局；`hashes_[]` 抽成独立 `std::array<uint32_t, kMaxConn>` 成员
  - `DpdkPoller<P>` 主模板**改为真特化**：直接 `std::array<P*, kMaxConn>` + 内联 `process_burst_()` 调用，不经过 `DpdkPoller<void>`
  - `DpdkPoller<void>` 保留为异构特化，做 `[[likely]]` + `rte_prefetch0` 标注
  - `lookup_by_5tuple_` 拆成两段：先扫 hashes_（cache-friendly），再 fallback 到 tuple 全比较
- `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp`：
  - `ReasmBuffer::append()` 失败时进入 stream error state（`state_ = Disconnected` + 触发 reconnect 信号），不 silent drop
  - `ReasmBuffer` 默认容量 64 KB → 256 KB
  - `StreamConfig::reasm_capacity` 在 DPDK 分支生效（当前是 kernel 分支有、DPDK 分支被忽略）
- 新增 `eph-net-dpdk/benchmarks/bench_dpdk_poller_dispatch.cpp` —— microbench 测量 Poller dispatch 开销（`DpdkPoller<void>` vs `DpdkPoller<DpdkTcpStream<Codec>>`），证明真特化收益
- 对应单测 `tests/unit/eph-net-dpdk/test_reasm_overflow_reconnect.cpp`（如果 test target 命名规则允许在 eph-net-dpdk/tests/ 下），或归并到既有 `test_dpdk_tcp_stream.cpp`

**Out of scope**:
- 不改 `on_message` / `on_datagram` 的 `std::function<>` 类型（决策：收益有限 ~5 ns/包，用户 ergonomics 冲击大；baseline 也是 std::function）
- 不改 WS 握手路径的 `staged_.insert` / `cipher_.insert`（控制面，控制面 spike 可接受）
- 不改 `ReasmBuffer::compact` 的 memmove 为 ring buffer（实测没看到周期性 spike，先保留）
- 不改 `send` 路径（当前已经是直接 mbuf 构造 + `rte_eth_tx_burst`，无回退）
- 不改 kernel 侧 `KernelPoller`（epoll_wait 语义不同，hash 预过滤和真特化都不适用）
- 不改 `StreamConfig` 的序列化格式或默认值语义（除 `reasm_capacity` 默认调整）
- 不引入新的 concept；`Poller` concept 本身不变
- 不改 `DpdkPoller::poll()` 的 `while (!stop_)` 外层循环——那是 Runtime 的事，Phase 13+

**Non-goals**:
- 不追求打败 baseline Reactor（baseline 单类型硬编码 `TcpSession<>*`，v3.3 `DpdkPoller<P>` 真特化理论上和它完全等价，不会显著超越）
- 不追求 `DpdkPoller<void>` 异构路径达到单类型性能——异构的多态代价（函数指针一跳）是设计必然

---

## 技术选型

| 类别 | 选择 | 理由 |
|---|---|---|
| Hash array 布局 | 独立 `std::array<uint32_t, kMaxConn>` 成员 | baseline Reactor 同款，cache-line 友好 |
| `DpdkPoller<P>` 特化策略 | 真特化（直接 `std::array<P*, kMaxConn>` + 编译期 inline）| 消除 function-pointer 一跳；和 baseline Reactor 等价 |
| 分支预测提示 | `[[likely]]` 在 "entry != nullptr" 和 "tuple 匹配" 分支 | baseline 有（reactor.hpp:245, 278, 354, 385）|
| Prefetch 策略 | `rte_prefetch0(mbufs[i+1])` 在 burst 循环里 | 掩盖下一个 mbuf 的 L3 header cache miss |
| ReasmBuffer 溢出响应 | 状态切 `Disconnected` + 返回 error | 上层 reconnect logic 接管，避免应用层数据丢失 |
| ReasmBuffer 默认容量 | 256 KB（4×当前）| 覆盖典型 L2 orderbook snapshot burst；内存代价 per-stream +192 KB 可接受 |
| `reasm_capacity` DPDK 分支支持 | 加一个 ctor 参数 | 和 kernel 分支对称 |
| 特化验证 | 新增 microbench `bench_dpdk_poller_dispatch` + 对比 | 硬数据驱动，避免"感觉应该快"式乐观 |

---

## 架构设计

### 模块划分

| 文件 | 类型 | 修改摘要 |
|---|---|---|
| `eph-net-dpdk/include/eph/net/dpdk/poller.hpp` | EDIT 大 | `PollableEntry` 瘦身，`hashes_[]` 独立成员，`DpdkPoller<P>` 真特化，`[[likely]]` + `prefetch` |
| `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp` | EDIT 中 | `ReasmBuffer::append` 失败→ error state，默认 capacity 256 KB，DPDK 分支尊重 `StreamConfig::reasm_capacity` |
| `eph-net-dpdk/include/eph/net/dpdk/config.hpp` | EDIT 小 | `StreamConfig::reasm_capacity` 字段加注释说明两个后端都尊重 |
| `eph-net-dpdk/benchmarks/bench_dpdk_poller_dispatch.cpp` | NEW | 对比 `DpdkPoller<void>` vs `DpdkPoller<DpdkTcpStream<Codec>>` 的 poll() 延迟分布 |
| `eph-net-dpdk/tests/test_reasm_overflow_reconnect.cpp` | NEW | 单测 reasm 溢出触发 error state + reconnect |

### 核心抽象

#### 1. 新 PollableEntry 布局（poller.hpp）

**当前**（`poller.hpp:297-307`）：
```cpp
struct PollableEntry {
    void*    obj;                // 8
    void   (*process_burst_fn)(void*, rte_mbuf**, uint16_t, uint64_t) noexcept;  // 8
    void   (*detach_fn)(void*) noexcept;   // 8
    void   (*tuple_fn)(void*, uint32_t*, uint32_t*, uint16_t*, uint16_t*) noexcept;  // 8
    uint32_t conn_hash;          // 4
    uint32_t src_ip, dst_ip;     // 8
    uint16_t src_port, dst_port; // 4
};  // ~48B + padding = 1 cache line / entry
```

**新**：
```cpp
// Per-entry heavy data (not scanned hot, 32B)
struct PollableEntry {
    void*    obj;                         // 8
    void   (*process_burst_fn)(void*, rte_mbuf**, uint16_t, uint64_t) noexcept;  // 8
    void   (*detach_fn)(void*) noexcept;  // 8
    uint32_t src_ip, dst_ip;              // 8
    uint16_t src_port, dst_port;          // 4
};

// Cache-friendly hot data (hashes only), separate dedicated array
std::array<uint32_t, kMaxConn> conn_hashes_{};   // 64B = 1 cache line for 16 entries
std::array<PollableEntry, kMaxConn> entries_{};  // heavy, only touched on hash match
std::size_t n_entries_{0};
```

`lookup_by_5tuple_` 变成两阶段：
```cpp
PollableEntry* lookup_by_5tuple_(rte_mbuf* mbuf) noexcept {
    // ... parse mbuf → pkt_hash, pkt tuple (unchanged)
    for (std::size_t i = 0; i < n_entries_; ++i) {
        if (conn_hashes_[i] != pkt_hash) continue [[likely]];
        auto& e = entries_[i];
        if (pkt_src_ip == e.dst_ip && pkt_dst_ip == e.src_ip &&
            pkt_src_port == e.dst_port && pkt_dst_port == e.src_port) [[likely]] {
            return &e;
        }
    }
    return nullptr;
}
```

Hash-only scan 16 条 entries 只读 `conn_hashes_` 一条 cache line（64 B = 16 × 4B）。Baseline 同款。

#### 2. `DpdkPoller<P>` 真特化

**当前**（`poller.hpp:369-395`，伪特化）：
```cpp
template <class P>
class DpdkPoller {
    std::unique_ptr<DpdkPoller<void>> impl_;  // heap, extra indirection
public:
    std::size_t poll() noexcept { return impl_->poll(); }  // 转发
    // ...
};
```

**新**：
```cpp
template <::eph::net::Pollable P>
class DpdkPoller {
public:
    static std::expected<std::unique_ptr<DpdkPoller>, core::ErrorInfo>
    create(PollerConfig cfg = {}) noexcept;

    std::expected<void, core::ErrorInfo> add(P* obj) noexcept;
    std::expected<void, core::ErrorInfo> remove(P* obj) noexcept;

    std::size_t poll() noexcept {
        if (n_entries_ == 0) [[unlikely]] return 0;
        rte_mbuf* mbufs[kBurstSize];
        const uint16_t n = rte_eth_rx_burst(cfg_.port_id, cfg_.rx_queue_id, mbufs, kBurstSize);
        if (n == 0) [[unlikely]] return 0;
        const uint64_t rx_tsc = eph::utils::TSC::now();
        std::size_t dispatched = 0;
        for (uint16_t i = 0; i < n; ++i) {
            if (i + 1 < n) rte_prefetch0(mbufs[i + 1]);
            P* obj = lookup_by_5tuple_(mbufs[i]);
            if (obj != nullptr) [[likely]] {
                obj->process_burst_(&mbufs[i], 1, rx_tsc);  // direct, inlined
                ++dispatched;
            } else {
                rte_pktmbuf_free(mbufs[i]);
            }
        }
        return dispatched;
    }

private:
    PollerConfig                        cfg_;
    std::array<uint32_t, kMaxConn>      conn_hashes_{};  // cache-hot
    std::array<P*, kMaxConn>            objs_{};
    std::array<ConnTuple, kMaxConn>     tuples_{};       // for collision check
    std::size_t                         n_entries_{0};

    P* lookup_by_5tuple_(rte_mbuf* mbuf) noexcept;  // similar to void version but P* direct
};
```

关键差异：
- `std::array<P*, kMaxConn> objs_` 替代 `PollableEntry.obj`，类型已知
- `obj->process_burst_(...)` 直接模板 method 调用，编译器 inline——**消除函数指针一跳**
- 不需要 `process_burst_fn` / `detach_fn` / `tuple_fn` 函数指针（都是 compile-time 已知类型）
- 和 baseline Reactor 的 `sess->process_rx<F>(...)` 直接调用一致

注意：P 的 `process_burst_` 是私有方法，目前只通过 friend 给 Poller 看。`DpdkPoller<P>` 真特化也要 friend 一遍。现在 `DpdkTcpStream` 里有 `friend class DpdkPoller<void>;` 和 `friend class DpdkPoller<DpdkTcpStream>;`（如果已有），需要检查加上。

#### 3. `DpdkPoller<void>` 的改动

仍然是主力路径（异构场景），但同样上 `[[likely]]` / prefetch / hash 分离：

```cpp
class DpdkPoller<void> {
public:
    std::size_t poll() noexcept {
        if (n_entries_ == 0) [[unlikely]] return 0;
        rte_mbuf* mbufs[kBurstSize];
        const uint16_t n = rte_eth_rx_burst(...);
        if (n == 0) [[unlikely]] return 0;
        const uint64_t rx_tsc = eph::utils::TSC::now();
        std::size_t dispatched = 0;
        for (uint16_t i = 0; i < n; ++i) {
            if (i + 1 < n) rte_prefetch0(mbufs[i + 1]);
            PollableEntry* entry = lookup_by_5tuple_(mbufs[i]);
            if (entry != nullptr) [[likely]] {
                entry->process_burst_fn(entry->obj, &mbufs[i], 1, rx_tsc);
                ++dispatched;
            } else {
                rte_pktmbuf_free(mbufs[i]);
            }
        }
        return dispatched;
    }
    // ...
};
```

相对改动：
- `conn_hashes_[]` 独立 array（从 `PollableEntry` 抽出）
- 加 `[[likely]]` / `[[unlikely]]`
- 加 `rte_prefetch0`
- 其它逻辑不变（保留 `process_burst_fn` 函数指针，因为异构性是卖点）

#### 4. `ReasmBuffer` 溢出处理（tcp_stream.hpp）

**当前**（`tcp_stream.hpp:114-143`，简化）：
```cpp
bool append(const uint8_t* p, std::size_t n) noexcept {
    if (writable() < n) {
        compact();
        if (writable() < n) return false;  // silent drop 上层只打 WARN
    }
    std::memcpy(buf_.data() + tail_, p, n);
    tail_ += n;
    return true;
}
```

**新**：
- `append()` 签名不变，返回 bool
- 调用处（`DpdkTcpStream::process_burst_` 内 reassembly 路径）在 `append()` 返回 false 时：
  1. 打 ERROR 级 log（`SPDLOG_LOGGER_ERROR(logger, "reasm buffer overflow cap={} need={} readable={}", buf_.size(), n, reasm_.readable())`)
  2. 设置 stream 状态 `state_ = TcpState::Error`（或新增 `ReasmOverflow` 子状态，看 `tcp_state.hpp` 既有枚举）
  3. 标记 `needs_reconnect_ = true`
  4. 不再尝试 decode（直接返回，mbuf 已 free）
- 上层 `Runtime::run_until` / bench scenario 主循环会看到 stream 进入 error 态，触发清理或 reconnect

**默认容量**：`ReasmBuffer(std::size_t cap = 256 * 1024)` 替代当前的 `= 64 * 1024`。

**`StreamConfig::reasm_capacity` 在 DPDK 分支生效**：检查当前 `DpdkTcpStream::create` 是否把 `cfg.reasm_capacity` 传给 `ReasmBuffer` 构造；如果没有，加。

#### 5. Microbench 新增

`eph-net-dpdk/benchmarks/bench_dpdk_poller_dispatch.cpp`：

```cpp
// 目标：测量纯 Poller dispatch 开销，隔离 rte_eth_rx_burst 实际网络 I/O
// 手法：mock rte_eth_rx_burst 返回预构造的 mbuf 数组；benchmark 只测 Poller 内部循环

static void BM_DpdkPoller_void(benchmark::State& state) {
    // 1 entry registered, fixed tuple, 32 mbufs prefilled
    // state.range(0) = number of entries (1, 4, 16)
    auto poller = DpdkPoller<void>::create(...);
    // ... setup
    for (auto _ : state) {
        poller->poll();
    }
}

static void BM_DpdkPoller_typed(benchmark::State& state) {
    auto poller = DpdkPoller<DpdkTcpStream<RawStreamCodec>>::create(...);
    // ... same setup
    for (auto _ : state) {
        poller->poll();
    }
}

BENCHMARK(BM_DpdkPoller_void)->Arg(1)->Arg(4)->Arg(16);
BENCHMARK(BM_DpdkPoller_typed)->Arg(1)->Arg(4)->Arg(16);
```

目标：typed 版本比 void 版本快 ≥ 3 ns/packet 在 N=1，≥ 5 ns/packet 在 N=16。

**前提 mock**：需要把 `rte_eth_rx_burst` 替换成一个测试桩，手动构造 mbuf 数组。可以用既有的 `test/dpdk_env.hpp` 提供的测试 infra。如果复杂度过高，microbench 改成 e2e bench（运行实际 NIC + 对比两种 Poller 跑 `lat_udp --dpdk` 时的 p50）。

---

## 接口设计

### 改动点

1. **`DpdkPoller<P>` ctor / add / remove / poll 签名不变**——用户代码透明升级。
2. **`DpdkPoller<void>` ctor / add / remove / poll 签名不变**。
3. **`ReasmBuffer::append` 签名不变**，返回语义不变（false = 失败），但调用处处理策略变化。
4. **`StreamConfig::reasm_capacity`** 字段已存在，只是确保 DPDK 分支真的使用它；无新字段。
5. **新增 `TcpState::Error` 或类似枚举值**（如果没有），在 `eph-net-kernel/include/eph/net/tcp_state.hpp`。待检查。

### 错误体系

- `ReasmBuffer::append` 返回 false → `DpdkTcpStream` 内部设 error state + 打 ERROR log
- Stream 用户通过既有的 `state()` API 检测到 `Error` 态，自行 close + reconnect
- 不引入新 Error enum 值（`core::Error::Disconnected` 复用）

---

## 编码规范

- `[[likely]] / [[unlikely]]` 只标真正的热路径分支（burst loop 内部的 `entry != nullptr` 和 `hash match`）
- `rte_prefetch0` 只标 burst 循环里的 "下一个 mbuf" prefetch
- `conn_hashes_` 成员用 `std::array<uint32_t, kMaxConn>` 值类型，非指针
- 保持头文件 only；新代码都 `inline` 或模板
- 日志级别：reasm overflow 是 ERROR；poll() hot path 不得打任何日志（TRACE 也不）
- `noexcept` 在 hot path 上一律标注
- 内存序：`n_entries_` 是 plain `std::size_t`（单线程 Poller，无需 atomic）
- 不加新的 `#include`（`<array>` 已包含，`rte_prefetch.h` 已随 DPDK header 包含）

---

## 实施计划

**Commit 策略**：单 commit，prefix `perf(dpdk):`。body 列出：改动文件、microbench 前后对比、对主要 bench scenario (lat_udp/lat_ex_md_udp --dpdk) 的 p50 影响。

### 阶段 12.0: eph-net-dpdk perf tuning（单 sub-phase）

**交付物**:

1. **EDIT** `eph-net-dpdk/include/eph/net/dpdk/poller.hpp`
   - `PollableEntry` 去掉 `conn_hash` 字段
   - 新增 `conn_hashes_` 独立 array 成员
   - `DpdkPoller<void>::lookup_by_5tuple_` 使用独立 hash array
   - `DpdkPoller<void>::poll` 加 `[[likely]]` + `rte_prefetch0`
   - `DpdkPoller<P>` 主模板重写为真特化（不再是 `unique_ptr<DpdkPoller<void>>` wrapper）

2. **EDIT** `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp`
   - `ReasmBuffer` 默认 capacity 64 KB → 256 KB
   - `ReasmBuffer::append` 失败时调用处设 error state
   - 加 ERROR 日志 with capacity/need/readable 上下文
   - `DpdkTcpStream::create` 或 ctor 把 `cfg.reasm_capacity` 真的传给 ReasmBuffer

3. **NEW** `eph-net-dpdk/benchmarks/bench_dpdk_poller_dispatch.cpp`
   - BM_DpdkPoller_void / BM_DpdkPoller_typed × N=1,4,16
   - 不依赖真实 NIC；用 mock burst generator

4. **NEW** `eph-net-dpdk/tests/test_reasm_overflow_reconnect.cpp`
   - 模拟 reasm 溢出场景（构造超过 `reasm_capacity` 的 frame bytes）
   - 验证 stream 进入 error state
   - 验证 ERROR 日志被打出（或验证 stream.state() 返回 Error）

5. **无** 其它改动。

**验收 gate**:

1. **Build**: `xmake build -g benchmarks -g tests` 全绿
2. **Unit tests**: `xmake run -g tests` 全绿，包括新增 `test_reasm_overflow_reconnect`
3. **Microbench (typed vs void)**:
   - `xmake run bench_dpdk_poller_dispatch` 产出对比数据
   - **要求**：`DpdkPoller<DpdkTcpStream<Codec>>::poll()` 比 `DpdkPoller<void>::poll()` **快 ≥ 3 ns/packet 在 N=1**，**快 ≥ 5 ns/packet 在 N=16**
   - 失败 → STATUS: BLOCKED（特化没生效或被其它 overhead 吞掉）
4. **End-to-end bench regression**:
   - `sudo lat tcp --dpdk` / `sudo lat udp --dpdk` / `sudo lat ex_md_udp --dpdk` 各跑一次 5 分钟
   - 与 Phase 11.1 commit `09df51b` 的数据对比（tcp dpdk 23015 / udp dpdk 20999 / ex_md_udp dpdk 22503）
   - **要求**：p50 不得变差 > 200 ns（允许 noise），理想应该持平或改进 50-500 ns
   - 失败 → 分析哪个改动引入回退，可能是 prefetch 过早 / cache 布局反效果，需要内部修正
5. **ReasmBuffer overflow test**：
   - `test_reasm_overflow_reconnect` 触发溢出场景，stream 进入 error state，log 出现 "reasm buffer overflow"
6. **无 silent drop**：手动审 `DpdkTcpStream::process_burst_` / drain_codec_ / reasm path，没有任何 `append` 返回 false 仍然继续运行的分支
7. **Deliverable checklist**：上面 1-6 全绿

**推荐 skill**: `/design auto`（单 subagent）

**预估**: ~200 LOC poller.hpp + ~80 LOC tcp_stream.hpp + ~150 LOC bench + ~100 LOC test ≈ 530 LOC。1 个 subagent session，~30 min wall clock。

---

## 关键决策记录

### D-1: ReasmBuffer 溢出时的响应策略

- **问题**：固定容量 reasm buffer 满时应该 (a) silent drop (b) grow on demand (c) 切 error state
- **选项**：
  - A. 现状 silent drop（保持）
  - B. 动态 grow（+ 运行时分配，违反 zero-alloc）
  - C. 进 error state → 触发 reconnect
  - D. 提高默认容量到 256KB 但保持 silent drop（只是降低概率）
- **决策**：C + 把默认从 64 KB 提到 256 KB
- **理由**：silent drop 在 HFT 是可靠性 bug，应用层数据丢失比链路断开危险。Reconnect 有成本但可控。256 KB 默认覆盖典型 L2 snapshot burst，减少日常触发 error state 的概率
- **验收**：`test_reasm_overflow_reconnect` 触发溢出，stream state 变 Error，有 ERROR log

### D-2: hashes_ 数组布局

- **问题**：hash 和 entry 数据混在一起 vs 独立 array
- **选项**：A 混在 PollableEntry 内（现状）/ B 独立 `conn_hashes_[]` 成员
- **决策**：B
- **理由**：baseline Reactor 同款。16 entries 的 hash-only 扫描从 touching 16 条 cache line → 1 条。对 N ≥ 8 场景明显
- **验收**：microbench `bench_dpdk_poller_dispatch` N=16 情况下 void 版本也有改善

### D-3: `DpdkPoller<P>` 伪特化改真特化

- **问题**：现有主模板是 `unique_ptr<DpdkPoller<void>>` wrapper，比直接用 void 更慢
- **选项**：
  - A. 保持伪特化（现状）
  - B. 删掉主模板只留 void 特化
  - C. 主模板改真特化（独立 `std::array<P*, kMaxConn>`，编译期 inline `P::process_burst_`）
- **决策**：C
- **理由**：真特化才是主模板的本意，伪特化是实现 bug。用户写 `DpdkPoller<DpdkTcpStream<Codec>>` 应该拿到 baseline Reactor 等价性能
- **验收**：microbench 证明 typed 比 void 快 ≥ 3 ns/packet N=1

### D-4: 不动 `std::function<>` on_message / on_datagram

- **问题**：是否改成函数指针 + user_data
- **选项**：A 不动 / B 改函数指针 / C template on callback type
- **决策**：A
- **理由**：
  - baseline 也是 std::function，没有性能回退
  - 每包 ~5 ns overhead，场景 1M pps 只有 5 ms/s CPU
  - 改成函数指针需要用户从 `lambda` 语法切 `void(*)(void*, ...)` + `void*` 手动 context，ergonomics 冲击大
  - C 会让 `DpdkTcpStream` 变成 double-template（Codec + OnMessage），模板元编程复杂度 ×2
- **验收**：N/A（不改动）

### D-5: compact memmove 不动

- **问题**：reasm compact 的 memmove 是否改 ring buffer
- **决策**：不动
- **理由**：Phase 11.1 lat_tcp dpdk 实测 p99.9=37 μs / stddev=2.4 μs，没看到周期性 spike，说明 compact 触发频率低。ring buffer 增加代码复杂度，收益不明。可以 Phase 13+ 加统计观察后再决定

### D-6: kernel KernelPoller 不在本 phase 改

- **问题**：kernel 侧是否做对称改动
- **决策**：不做
- **理由**：
  - `KernelPoller` 走 epoll_wait，没有 5-tuple hash lookup（kernel TCP 栈处理了），`hashes_[]` 独立 array 不适用
  - `KernelPoller::poll` 循环遍历 entries_ 也相对简单，没有 burst dispatch 的 cache footprint 问题
  - 真特化 `KernelPoller<P>` 可以加但现在也没人用，ROI 低
- **验收**：N/A

### D-7: Microbench 用 mock mbuf 还是真 NIC

- **问题**：dispatch microbench 依赖真实 NIC（复杂 setup）还是 mock mbuf 数组
- **选项**：A mock mbuf + mocked rte_eth_rx_burst / B 真 NIC + sudo / C e2e lat bench 对比
- **决策**：A（优先）+ C 作为交叉验证
- **理由**：
  - microbench 目标是隔离 poll() 内部开销，真 NIC 会把网络 jitter 塞进来噪声 > 信号
  - mock mbuf 可以在 build/linux/arm64/release/bench_dpdk_poller_dispatch 直接跑，无 sudo，CI 友好
  - 如果 mock 复杂度过高（EAL/Platform 初始化绕不开），降级到 C：只跑 lat_udp --dpdk 对比 commit 前后 p50
- **验收**：C 路径的 e2e 数据必须出，A 路径作为加分项

---

## 一致性检查

- ✅ D-1 (ReasmBuffer error state) 与 Out-of-scope "不改 state machine" 看似矛盾，实际只是复用既有 Error 态，不增新 state
- ✅ D-2 (hash 独立 array) 与 "只改 poller.hpp 不碰 concept" 一致
- ✅ D-3 (真特化) 与 "Poller concept 不变" 一致——特化实现改但 concept 接口不变
- ✅ D-4 (不动 std::function) 与 Out of scope 显式列出的"不改 on_message"一致
- ✅ D-5 (compact 不动) 与 In-scope 列表一致（未列出 ring buffer）
- ✅ D-6 (kernel 不改) 与 Out-of-scope 列出"不改 kernel"一致
- ✅ Microbench 数据作为 gate 3 硬性要求，可证明/证伪改动收益
- ✅ E2E bench 作为 gate 4 回归守门，防止 prefetch / cache 布局反效果
- ✅ 整个 phase 改动面小（~530 LOC）一次 subagent 可完成

---

## 执行说明

配合 `/repeat` subagent 模式执行。命令示例：

```
/design auto 按 .artifacts/plan-phase-12-dpdk-perf-tuning-20260411-132857.md
执行阶段 12.0。严格遵循 plan：
- 只动 poller.hpp + tcp_stream.hpp + 新增 1 bench + 1 test
- 不动 on_message / on_datagram std::function
- 不动 ReasmBuffer::compact
- 不动 KernelPoller
- 单 commit，prefix perf(dpdk):
完成后运行 7 条 gate。microbench 特化收益 < 3 ns/packet (N=1) 或 5 ns/packet (N=16) → STATUS: BLOCKED。
E2E p50 回退 > 200 ns → BLOCKED。不放宽任何阈值。
环境：gcc14 wrapper 在 /tmp/gcc14-wrap/，export PATH=/tmp/gcc14-wrap:$PATH。
必读 plan + .artifacts/review-eph-net-dpdk-perf-20260411-130628.md +
.temp/baseline-pre-v3.3/eph-dpdk/include/eph/dpdk/reactor.hpp 作为 baseline 参考。
```
