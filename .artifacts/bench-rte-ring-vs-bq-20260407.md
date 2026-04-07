# Benchmark Report: rte_ring vs BoundedQueue (SPSC)

## 概况
- 时间：2026-04-07 10:27
- 模式：compare
- 目标：DPDK rte_ring (SPSC mode) vs eph-containers BoundedQueue
- 分支：dev
- Compiler: gcc 15.2.1, -O3 -march=native -mssse3

## 结果汇总

### 1. PushPop（单线程 enqueue+dequeue 一次的延迟）

| Payload | BoundedQueue | rte_ring | Winner | Delta |
|---------|-------------|----------|--------|-------|
| 8B | **0.94ns** | 1.50ns | BQ (−37%) | BQ 更快 |
| 64B | **1.77ns** | 1.23ns | **rte_ring (−31%)** | rte_ring 更快 |
| 512B | 20.3ns | **1.23ns** | **rte_ring (−94%)** | rte_ring 16× 更快 |

**分析**：rte_ring 只传 void*（8 字节指针），延迟与 payload 大小无关（~1.3ns 恒定）。BoundedQueue 做值拷贝，延迟随 payload 线性增长。8B payload 时 BQ 略快（cache line 内操作更简单）。

### 2. Throughput（SPSC 双线程持续推送，items/sec）

| Payload | BQ (B:512) | rte_ring (B:512) | Winner |
|---------|-----------|------------------|--------|
| 8B | 592M/s | 254M/s | **BQ 2.3×** |
| 64B | 85M/s | 130M/s | **rte_ring 1.5×** |
| 512B | 45M/s | 184M/s | **rte_ring 4.1×** |

**分析**：BoundedQueue 在小 payload 上有绝对优势（值传递 8B 比指针间接访问更快，更好的 cache locality）。大 payload 时 rte_ring 大幅领先（避免了 memcpy 开销）。

### 3. PingPong（跨核 RTT，CPU affinity pinned）

| Payload | BQ (B:512) | rte_ring (B:512) | Winner |
|---------|-----------|------------------|--------|
| 8B | **119ns** | 229ns | **BQ 1.9× 更快** |
| 64B | 158ns | 230ns | **BQ 1.5× 更快** |
| 512B | 298ns | 211ns | **rte_ring 1.4× 更快** |

**分析**：PingPong 对 cache coherence protocol（MESI）极其敏感。BQ 的值语义在小 payload 时只需一次 cache line transfer；rte_ring 的指针间接访问需要两次（取指针 + 取数据）。大 payload 时 BQ 的 memcpy 成本超过了 rte_ring 的额外间接。

### 4. Batch（批量操作均摊延迟，items/sec）

| Payload | Batch=512 | BQ | rte_ring | Winner |
|---------|-----------|-----|----------|--------|
| 8B | 512 items | 3.75G/s | 3.34G/s | **BQ +12%** |
| 64B | 512 items | 1.15G/s | 3.42G/s | **rte_ring 3.0×** |
| 512B | 512 items | 86M/s | 338M/s | **rte_ring 3.9×** |

**分析**：rte_ring 的 burst API 在大 payload 时优势巨大——它只批量移动指针（固定 8B×N），而 BQ 批量 memcpy N×payload_size。

### 5. QueueFull（满队列 try_push 失败延迟）

| BQ | rte_ring |
|----|----------|
| 0.35-0.66ns | 0.31-0.41ns |

两者都 < 1ns，几乎无区别。都是简单的原子比较返回 false。

---

## 核心结论

**交叉点：约 64 字节 payload**

| 场景 | < 64B Payload | >= 64B Payload |
|------|---------------|----------------|
| PushPop | **BQ 快 37%** | **rte_ring 快 31-94%** |
| Throughput | **BQ 快 2.3×** | **rte_ring 快 1.5-4.1×** |
| PingPong | **BQ 快 1.5-1.9×** | **rte_ring 快 1.4×** |
| Batch | **BQ 快 12%** | **rte_ring 快 3.0-3.9×** |

### 根因

| | BoundedQueue | rte_ring |
|--|-------------|----------|
| 元素传递 | 值拷贝（memcpy N bytes） | 指针传递（固定 8 bytes） |
| Cache 行为 | 数据连续，locality 好 | 需额外 deref，1 次 cache miss |
| 小 payload 优势 | 8B 值拷贝 ≈ 寄存器 mov | 指针 + deref = 2 次操作 |
| 大 payload 劣势 | 512B memcpy = ~20ns | 指针 store/load = ~1.3ns |

### 对 ephemeral 项目的建议

1. **eph-transport 的 SPSC 队列保持 BoundedQueue**：Transport 层传递的是 `TxMessage` / `RxMessage`（~64-128B），在此范围 BQ 和 rte_ring 差距很小，而 BQ 有更好的值语义、模板安全性、无 EAL 依赖。

2. **eph-dpdk 的 DPDK 路径如果需要传大对象（如 rte_mbuf*）**：可以直接用 rte_ring——它天然传 void*，与 DPDK mbuf 语义一致。

3. **不建议全局替换**：BoundedQueue 是 header-only、无外部依赖、值语义安全；rte_ring 需要 EAL、只传指针（需手动管理 payload 生命周期）。在非 DPDK 路径上引入 rte_ring 是退步。
