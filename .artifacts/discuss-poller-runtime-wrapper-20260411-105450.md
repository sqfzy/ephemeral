# Discussion Record

## Context

- **时间**：2026-04-11 10:54:50
- **耗时**：~15 分钟
- **用户原始需求**：Poller 的上层封装还不够。用户不应该手动 poll，就像 tokio 不会让用户手动 poll。同时我们需要保证低延迟，零成本。你的方案是？
- **复杂度评估**：高（HFT 库 API 设计，低延迟 vs 人体工学双目标，多技术路径各有拥趸）
- **讨论轮数**：7 轮
- **参与角色**：R2 极简主义者 / R3 性能狂热者 / R8 激进创新者 / R14 架构师 / R7 用户代言人 / R6 维护性倡导者（全部预定义）

## 内容摘要

R8 初期推 C++20 coroutines 作为 "Tokio 灵魂"，被 R3 的 HALO 实测数据（gcc 14 aarch64 在简单 echo loop 触发率 ~70%、复杂 reconnect loop ~20%）打回热路径不可行。R14 提出分阶段：第一阶段只做薄封装 `Runtime<P>`，第二阶段 coroutine 作为冷路径独立 helper。R7 补齐 bench 场景的 `run_until(pred)` 需求，R2 简化为 ~80 LOC 模板化实现，R3 守住零成本底线（值类型 Pred、std::atomic acquire、独立 overhead bench），R6 要求文档一并迁移。最终共识：**本次决议产出第一阶段 `BasicRuntime<P>` 薄封装（attach/run/run_for/run_until/stop/reset 六方法），6 bench scenarios 一次性迁移，新增 `bench_runtime_overhead` 证明零成本。coroutine 第二阶段独立 discuss。**

---

## 上下文事实（来自代码）

- 当前 `eph::net::Poller` concept 三方法：`add/remove/poll`
- 三实现：`KernelPoller`（epoll_wait，支持 timeout）/ `DpdkPoller<>`（burst poll 非阻塞）/ `TestPoller<P>`
- Pollable 有 `on_message`/`on_datagram` 回调，`send()` 同步
- 典型用户代码：`while (running) poller->poll();` + 手动装 SIGINT + 手动 shutdown flag（`bench/latency` 6 scenarios 全部这样写，~20 行样板每个）
- **没有** Runtime / block_on / spawn / co_await / Task / Future 任何东西
- 类型擦除走函数指针表 "P2"，零 vtable 零 std::function

---

## 完整讨论

（7 轮发言内容见对话记录上方，此处略——所有角色盒子按顺序保留在 `/discuss` 原始输出中）

---

## 最终方案

### 第一阶段交付物

**新增文件** `eph-net/include/eph/net/runtime.hpp` (~80 LOC header-only):

```cpp
namespace eph::net {

template <class P>
concept Runtime = requires(P& p, /*Pollable*/ auto* q) {
    { p.attach(q) }  -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { p.detach(q) }  -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { p.run() };
    { p.stop() };
};

template <Poller P>
class BasicRuntime {
    P poller_;
    std::atomic<bool> stop_{false};

public:
    static std::expected<BasicRuntime, core::ErrorInfo>
    create(typename P::Config cfg = {}) noexcept;

    template <Pollable Q> auto attach(Q* q) { return poller_.add(q); }
    template <Pollable Q> auto detach(Q* q) { return poller_.remove(q); }

    void run() {
        install_signal_handler_once();
        while (!stop_.load(std::memory_order_acquire)) {
            poller_.poll();
        }
    }

    void run_for(std::chrono::nanoseconds d);

    template <typename Pred>
    requires std::invocable<Pred&> && std::convertible_to<
        std::invoke_result_t<Pred&>, bool>
    void run_until(Pred p) {
        while (!stop_.load(std::memory_order_acquire) && p()) {
            poller_.poll();
        }
    }

    void stop() noexcept { stop_.store(true, std::memory_order_release); }
    void reset() noexcept { stop_.store(false, std::memory_order_release); }

    P& poller() noexcept { return poller_; }  // escape hatch
};

using KernelRuntime = BasicRuntime<kernel::KernelPoller>;
template <Pollable... Ps>
using DpdkRuntime = BasicRuntime<dpdk::DpdkPoller<Ps...>>;

} // namespace eph::net
```

### 迁移

`bench/latency/{tcp,udp,ws,exchange}/lat_*.cpp` 12 个二进制（6 scenario × 2 backend）全部改用 `Runtime` API，同一 commit 内完成，旧 `while (running) poller->poll();` 模板消失。预计 -120 样板行，+80 Runtime + +30 使用。

### 文档

`docs/poller-guide.md` → `docs/runtime-guide.md`（rename + rewrite），README 示例同步。`Poller` 作为底层 API 留下但标"推荐使用 Runtime"。

### 验证

新增 `bench_runtime_overhead.cpp`，对比 `while { poll(); }` vs `rt.run_until(pred)` 的 p50/p99/p999 RDTSC delta；任何 ≥ 5 ns 差异被视为 regression 要复查汇编。

### 第二阶段（未来独立讨论，不在本次决议）

- `eph/net/async.hpp` header：`Task<T>` + `co_await` helpers 用于冷路径（reconnect、WS handshake、订单重发）
- 独立 xmake target，默认关闭
- Runtime 第一阶段 API 保证不锁死 "`block_on(Task<T>)` 扩展路径"——即方法 non-final、类 non-final

---

## 已解决的分歧

| 分歧点 | 解决方式 | 关键论据 |
|---|---|---|
| coroutine 要不要现在加 | 分阶段，第一阶段只做薄封装 | R8 HALO 实测 ~70% simple / ~20% complex，hot path 不可接受 |
| `spawn` / executor 层要不要 | 不要 | eph 单 pipeline 用法，executor 无工作可做 |
| atomic bool vs volatile sig_atomic_t | `std::atomic<bool>` + `acquire/release` | 和 `benchmarks/latency/core/measurement.hpp:71-82` 既有约定一致 |
| `run_until` 的 Pred 类型 | 值类型 callable 模板参数 | std::function 有 heap alloc |
| Runtime 要不要 concept | 要 | 概念分层防止未来 `run()` 被塞进 Poller 破坏单一职责 |
| Runtime 要不要 template 化 | 要，`BasicRuntime<P>` 一份实现 | Kernel/DPDK 的 Runtime 逻辑完全相同 |
| send 要不要 async | 不要 | DPDK/kernel TX 是 ~0.5 μs / 100 ns 同步操作，async 只加 overhead |
| bench 迁移要不要过渡期 | 不要，一次性迁移 | R7 主张避免"新老共存永远不结束" |
| 文档迁移是不是可选 | 是交付物不是 todo | R6 反对用户发现不了 Runtime |

## 未解决的权衡（需用户决策）

| 冲突 | 倾向保留 | 倾向删除 | 建议 |
|---|---|---|---|
| `BasicRuntime::poller()` escape hatch | R14 / R7 | R2 | 第一阶段保留，@deprecated "仅用于 test fixture"；6 个月内 0 使用即删 |
| `run_for(duration)` 精度 | R7 要 ns | R3 说 μs 够 | 入参 `std::chrono::nanoseconds`，实际精度由 `monotonic_raw_ns` 决定，不做承诺 |

## 会议摘要

- 参与角色：R2 / R3 / R8 / R14 / R7 / R6
- 讨论轮数：7
- 主要争议：coroutine 零成本可达性；executor 层必要性；Runtime 抽象最小充分形式
- 收敛路径：HALO 数据 → 分阶段 → run_until 补齐 → 模板化 → 零成本验证 → 文档迁移 → 定型 API
- 最终共识：第一阶段薄封装 `BasicRuntime<P>` (~80 LOC) + 6 scenario 一次性迁移 + overhead bench；第二阶段 coroutine 独立 discuss
