# Plan: rte_ring vs BoundedQueue Benchmark

> 在 HFT SPSC 场景下对比 DPDK rte_ring 和自研 BoundedQueue 的延迟与吞吐量，为是否迁移提供数据支撑。

创建时间：2026-04-05
状态：已完成

---

## 定位与边界

**目标**：用数据回答"rte_ring 是否比 BoundedQueue 延迟更低？"——如果差距显著，考虑在 DPDK 路径上用 rte_ring 替代；如果差距不大或 BoundedQueue 更快，保持现状。

**In scope**：
- SPSC 模式下的 5 个场景对比
- rte_ring 使用完整 EAL init（`--no-huge --no-shconf`）
- 与现有 bench_matrix 相同的 payload/buffer 组合

**Out of scope**：
- MPMC / multi-producer / multi-consumer 对比
- EvictingQueue 对比（rte_ring 无语义等价物）
- 生产代码迁移（本次只产出数据）

---

## 架构设计

### 文件位置

`eph-dpdk/benchmarks/bench_rte_ring_vs_bq.cpp`

一个文件内包含 BoundedQueue 和 rte_ring 的同场景实现，Google Benchmark 同进程运行消除系统性偏差。

### EAL 初始化

在 `main()` 之前通过 Google Benchmark 的 custom main 或全局 fixture 执行：

```cpp
// Minimal EAL init — no hugepages, no shared config, single lcore
static struct EalInit {
    EalInit() {
        const char* argv[] = {"bench", "--no-huge", "--no-shconf", "-l", "0", nullptr};
        rte_eal_init(5, const_cast<char**>(argv));
    }
} g_eal_init;
```

### rte_ring SPSC 配置

```cpp
auto* ring = rte_ring_create("bench_ring", capacity,
                              SOCKET_ID_ANY,
                              RING_F_SP_ENQ | RING_F_SC_DEQ);
```

- `RING_F_SP_ENQ | RING_F_SC_DEQ`：Single-Producer Single-Consumer 模式
- capacity 必须是 2^N（与 BoundedQueue 的 Capacity 模板参数相同）
- 元素类型：`void*`（rte_ring 只能传指针），需要一个 pre-allocated pool of Payload objects 来模拟值传递

### Payload 适配

BoundedQueue 直接存储值类型（`Payload<N>`），rte_ring 只能传 `void*`。为公平对比：

```cpp
// Pre-allocate payload pool (避免热路径上的 malloc)
Payload<N> payload_pool[BufSize];
size_t pool_idx = 0;

// Enqueue: 传指针
Payload<N>* p = &payload_pool[pool_idx++ & (BufSize - 1)];
rte_ring_sp_enqueue(ring, p);

// Dequeue: 收指针后 memcpy 到 local
void* out_ptr;
rte_ring_sc_dequeue(ring, &out_ptr);
auto* out = static_cast<Payload<N>*>(out_ptr);
benchmark::DoNotOptimize(*out);
```

这确保两者都做了等价的内存操作——BoundedQueue 的 memcpy-into-slot 对应 rte_ring 的 pointer store + dequeue 后 deref。

### Batch 适配

rte_ring 有原生 `rte_ring_sp_enqueue_burst()` / `rte_ring_sc_dequeue_burst()`。BoundedQueue 有 `try_push_n()` / `try_consume_all()`。直接对比 batch 接口。

---

## 接口设计

### 5 个 Benchmark 场景

**1. PushPop（单线程 push+pop round-trip）**
```cpp
BM_BQ_PushPop<PayloadSize, BufSize>      // BoundedQueue baseline
BM_RteRing_PushPop<PayloadSize, BufSize> // rte_ring comparison
```
测量单线程下 enqueue+dequeue 一个元素的延迟。

**2. Throughput（SPSC 双线程单向吞吐量）**
```cpp
BM_BQ_Throughput<PayloadSize, BufSize>
BM_RteRing_Throughput<PayloadSize, BufSize>
```
Producer 线程持续 push，Consumer 线程持续 pop，测量整体吞吐。

**3. PingPong（跨核延迟 RTT）**
```cpp
BM_BQ_PingPong<PayloadSize, BufSize>
BM_RteRing_PingPong<PayloadSize, BufSize>
```
T1→Q1→T2→Q2→T1 round-trip，CPU affinity pinning，测量 MESI cache-line transfer 延迟。

**4. Batch（批量 enqueue/dequeue 吞吐量）**
```cpp
BM_BQ_Batch<PayloadSize, BufSize>
BM_RteRing_Batch<PayloadSize, BufSize>
```
批量操作 N 个元素，测量均摊延迟。rte_ring 使用 burst API。

**5. QueueFull（队列满时 enqueue 行为）**
```cpp
BM_BQ_TryPushFull<PayloadSize, BufSize>
BM_RteRing_TryEnqueueFull<PayloadSize, BufSize>
```
队列已满时 try_push / rte_ring_sp_enqueue 的失败路径延迟。

### Payload/Buffer Matrix

沿用 `bench_matrix.hpp` 的 REGISTER_MATRIX 宏（12 组合）：
- Payload: 8B, 64B, 512B
- Buffer: 1, 8, 64, 512 slots

PingPong 场景需要 `->Threads(2)` 并跳过 BufSize=1（ping-pong 需要至少 2 个 slot 或 2 个独立队列）。

---

## 实施计划

### 阶段 1: 创建 benchmark 文件

- 编写 `eph-dpdk/benchmarks/bench_rte_ring_vs_bq.cpp`
- 全局 EAL init（--no-huge --no-shconf）
- rte_ring 创建/销毁辅助函数
- Payload pool 分配
- 5 个场景 × 2 个实现 = 10 个 benchmark 函数模板
- REGISTER_MATRIX 注册全部组合
- 交付物：编译通过 + 运行输出结果
- 验收：BM_BQ_* 和 BM_RteRing_* 同时出现在输出中
- 预估：2-3 小时

### 阶段 2: 运行 + 分析

- 以 release mode 运行（`-O3 -march=native -mssse3`）
- 保存原始输出到 `.artifacts/bench-data-rte-ring-vs-bq-YYYYMMDD.txt`
- 生成对比表格和分析报告
- 交付物：`.artifacts/bench-rte-ring-vs-bq-YYYYMMDD.md` 含结论和建议
- 验收：每个场景都有 BQ vs rte_ring 的数据和百分比差异
- 预估：1 小时

---

## 关键决策记录

### D-1: rte_ring 元素类型适配
- **问题**：rte_ring 只能传 void*，BoundedQueue 传值类型
- **决策**：使用 pre-allocated payload pool，enqueue 传指针，dequeue 后 deref
- **理由**：消除 malloc 噪声，保持热路径上的内存访问模式一致
- **验收标准**：两者在每次 push/pop 中执行的 memcpy 字节数等价

### D-2: EAL init 方式
- **问题**：rte_ring_create 需要 EAL
- **决策**：--no-huge --no-shconf -l 0 最小化 init
- **理由**：无需 root/hugepages，global state 在 bench 进程结束时自然清理
- **验收标准**：bench 可在非特权用户下运行
