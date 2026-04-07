# Plan: rte_memcpy + rte_ring 替换对 E2E 延迟影响测试

> 用编译时开关替换 eph-dpdk 热路径的 memcpy 和 Transport SPSC 队列，用 bench_latency.sh 测量端到端延迟变化。

创建时间：2026-04-07
状态：已确认

---

## 定位与边界

**目标**：量化 rte_memcpy 和 rte_ring 在真实 E2E 场景下的延迟改善幅度，为是否在 DPDK 路径上采纳提供数据。

**In scope**：
- rte_memcpy 替换 eph-dpdk/tcp.hpp 中的 std::memcpy（实验 A）
- rte_ring_elem 替换 DPDK Transport 的 BoundedQueue SPSC 队列（实验 B）
- 编译时开关控制（`EPH_USE_RTE_MEMCPY` / `EPH_USE_RTE_RING`）
- 用 bench_latency.sh 测量 market data P50/P99 和 order RTT

**Out of scope**：
- 替换 eph-transport/eph-net 中的 memcpy（无 DPDK 依赖）
- 替换非 DPDK 路径的 BoundedQueue
- 生产代码永久合入（取决于 bench 结果）

---

## 架构设计

### 实验 A: rte_memcpy（1 行改动）

文件：`eph-dpdk/include/eph/dpdk/tcp.hpp:956`

```cpp
// Before:
std::memcpy(entry.data, parsed.payload, parsed.payload_len);

// After:
#ifdef EPH_USE_RTE_MEMCPY
rte_memcpy(entry.data, parsed.payload, parsed.payload_len);
#else
std::memcpy(entry.data, parsed.payload, parsed.payload_len);
#endif
```

需要 `#include <rte_memcpy.h>`（已在 eph-dpdk 的 include 路径中）。

### 实验 B: RteRingQueue 包装类

新文件：`eph-dpdk/include/eph/dpdk/rte_ring_queue.hpp`

包装 `rte_ring_elem` API 匹配 BoundedQueue 接口：

```cpp
namespace eph::dpdk {

/// Drop-in replacement for BoundedQueue using DPDK rte_ring_elem API.
/// Requires EAL initialization before construction.
template <typename T, size_t Capacity>
class RteRingQueue {
public:
    RteRingQueue() {
        static std::atomic<uint64_t> counter{0};
        auto name = std::format("eph_rq_{}", counter.fetch_add(1));
        // rte_ring requires power-of-2 count; Capacity already is (BoundedQueue same constraint)
        ring_ = rte_ring_create_elem(name.c_str(), sizeof(T),
                                      Capacity, SOCKET_ID_ANY,
                                      RING_F_SP_ENQ | RING_F_SC_DEQ);
    }
    ~RteRingQueue() { if (ring_) rte_ring_free(ring_); }

    // Move only (ring_ is a C resource)
    RteRingQueue(RteRingQueue&& o) noexcept : ring_(o.ring_) { o.ring_ = nullptr; }
    RteRingQueue& operator=(RteRingQueue&& o) noexcept {
        if (this != &o) { if (ring_) rte_ring_free(ring_); ring_ = o.ring_; o.ring_ = nullptr; }
        return *this;
    }
    RteRingQueue(const RteRingQueue&) = delete;
    RteRingQueue& operator=(const RteRingQueue&) = delete;

    [[nodiscard]] bool try_push(const T& val) noexcept {
        return rte_ring_sp_enqueue_elem(ring_, &val, sizeof(T)) == 0;
    }

    [[nodiscard]] bool try_pop(T& val) noexcept {
        return rte_ring_sc_dequeue_elem(ring_, &val, sizeof(T)) == 0;
    }

    // Blocking push/pop (spin-wait, matching BoundedQueue semantics)
    void push(const T& val) noexcept { while (!try_push(val)) {} }
    void pop(T& val) noexcept { while (!try_pop(val)) {} }

    // produce/consume (zero-copy callback interface)
    // rte_ring doesn't support in-place mutation, so we copy via temp
    template <typename F>
    [[nodiscard]] bool try_produce(F&& writer) noexcept {
        T tmp{};
        writer(tmp);
        return try_push(tmp);
    }

    template <typename F>
    [[nodiscard]] bool try_consume(F&& reader) noexcept {
        T tmp;
        if (!try_pop(tmp)) return false;
        reader(tmp);
        return true;
    }

    template <typename F>
    void produce(F&& writer) noexcept {
        T tmp{};
        writer(tmp);
        push(tmp);
    }

    template <typename F>
    void consume(F&& reader) noexcept {
        T tmp;
        pop(tmp);
        reader(tmp);
    }

    [[nodiscard]] size_t size() const noexcept {
        return rte_ring_count(ring_);
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

    [[nodiscard]] bool empty() const noexcept { return rte_ring_empty(ring_); }

private:
    rte_ring* ring_ = nullptr;
};

} // namespace eph::dpdk
```

### 注入到 Transport

DPDK 类型别名文件 `eph-dpdk/include/eph/dpdk/types.hpp` 中，根据编译开关选择队列类型：

```cpp
#ifdef EPH_USE_RTE_RING
#include "eph/dpdk/rte_ring_queue.hpp"
template <typename T, size_t N>
using DpdkQueueTemplate = eph::dpdk::RteRingQueue<T, N>;
#else
template <typename T, size_t N>
using DpdkQueueTemplate = eph::containers::BoundedQueue<T, N>;
#endif

// 现有类型别名改为使用 DpdkQueueTemplate
using DpdkTransport = eph::net::Transport<TcpSession<>,
    eph::net::WsFramer, 512, 1024, DpdkQueueTemplate>;
```

**问题**：Transport 模板的 RxQueueTmpl 参数需要确认签名。让我检查。

### 编译开关

xmake option:
```lua
option("use_rte_memcpy")
    set_default(false)
    set_showmenu(true)
    add_defines("EPH_USE_RTE_MEMCPY")

option("use_rte_ring")
    set_default(false)
    set_showmenu(true)
    add_defines("EPH_USE_RTE_RING")
```

构建对比版本：
```bash
# 基线（当前代码）
xmake f -m release && xmake build bench_market_dpdk bench_order_rtt_dpdk

# 替换版本
xmake f -m release --use_rte_memcpy=y --use_rte_ring=y && xmake build bench_market_dpdk bench_order_rtt_dpdk
```

---

## 实施计划

### 阶段 1: 基础设施（编译开关 + RteRingQueue）

- 1a. xmake.lua 添加 `use_rte_memcpy` 和 `use_rte_ring` option
- 1b. 创建 `eph-dpdk/include/eph/dpdk/rte_ring_queue.hpp`
- 1c. `tcp.hpp:956` 添加 `#ifdef EPH_USE_RTE_MEMCPY` 条件
- 1d. `types.hpp` 添加 `#ifdef EPH_USE_RTE_RING` 条件类型别名
- 1e. 验证两种模式都能编译：`xmake f --use_rte_memcpy=y --use_rte_ring=y && xmake build bench_market_dpdk`
- 交付物：两种模式编译通过
- 预估：2-3 小时

### 阶段 2: 运行 bench_latency.sh + 分析

- 2a. 编译基线版本，运行 bench_latency.sh，保存结果
- 2b. 编译替换版本，运行 bench_latency.sh，保存结果
- 2c. 对比分析，生成报告
- 交付物：`.artifacts/bench-rte-replacement-YYYYMMDD.md` 含对比数据
- 预估：依赖硬件环境
- **注：需要双 NIC 环境（AWS EC2）。本地 WSL 无法运行此 bench。**

---

## 关键决策记录

### D-1: rte_ring_elem vs void* + 对象池
- **决策**：rte_ring_elem（直接传值）
- **理由**：语义与 BoundedQueue 完全一致，无对象池复杂度，DPDK 21.11+ 原生支持

### D-2: 编译开关 vs 分支隔离
- **决策**：编译时开关（`#ifdef`）
- **理由**：允许同一 codebase 编译两种版本，便于 A/B 对比

### D-3: 替换范围
- **决策**：仅 DPDK 路径（eph-dpdk）
- **理由**：eph-transport/eph-net 不依赖 DPDK，引入 rte_memcpy/rte_ring 会破坏模块独立性
