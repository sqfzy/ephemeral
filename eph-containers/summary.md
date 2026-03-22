# eph-containers 项目总结

## 1. 概述

eph-containers 是 ephemeral 项目中的高性能无锁容器库，使用 C++23 编写，采用 header-only 风格。该库提供了两大类 SPSC（Single-Producer Single-Consumer）无锁队列：**有界队列（BoundedQueue）** 和 **可丢弃队列（EvictingQueue）**，并各自附带面向字节流的适配器变体（BoundedQueueBytes / EvictingQueueBytes）。

所有容器均针对 CPU-pinned 线程间低延迟通信场景设计。核心设计理念包括：cache line 对齐避免 false sharing、影子索引减少跨核原子读取、2 的幂容量实现位掩码取模、零拷贝 Visitor 模式避免中间拷贝。数据类型受 `TrivialData` concept 约束——必须是 trivially copyable 且 default initializable 的类型。

库依赖同项目的 `eph-utils` 包，使用其中的 `Align<T>` cache line 对齐工具和 `cpu_relax()` 自旋等待指令（x86 PAUSE / ARM64 YIELD）。整个库无外部第三方依赖，纯标准库 + 内联汇编。

## 2. 架构

```
+-------------------------------------------------------+
|                  eph/containers.hpp                    |
|              (Convenience umbrella header)             |
+-------------------------------------------------------+
        |           |            |            |
        v           v            v            v
+-------------+ +----------+ +-------------+ +----------+
| bounded_    | | evicting_| | bounded_    | | evicting_|
| queue.hpp   | | queue.hpp| | queue_      | | queue_   |
|             | |          | | bytes.hpp   | | bytes.hpp|
| BoundedQueue| | Evicting | | BoundedQueue| | Evicting |
| <T,Cap>     | | Queue    | | Bytes       | | Queue    |
|             | | <T,Cap>  | | <Max,Cap>   | | Bytes    |
+------+------+ +----+-----+ +------+------+ +----+-----+
       |              |              |              |
       v              v              |              |
  +----------+   +----------+       |              |
  |concepts. |   |concepts. |  (wraps BoundedQueue) |
  |hpp       |   |hpp       |       |    (wraps EvictingQueue)
  +----------+   +----------+       |              |
       |              |              v              v
       v              v         +----------+  +----------+
  +---------+   +---------+    |bounded_  |  |evicting_ |
  |eph-utils|   |eph-utils|    |queue.hpp |  |queue.hpp |
  |alignment|   |cpu.hpp  |    +----------+  +----------+
  +---------+   +---------+
```

## 3. 模块映射

| 模块/文件 | 职责 | 关键类型 | 依赖 |
|-----------|------|----------|------|
| `include/eph/containers.hpp` | 便捷 umbrella header，包含所有子模块 | (无) | 所有子模块 |
| `include/eph/containers/concepts.hpp` | 定义数据类型约束 | `TrivialData<T>` concept | `<concepts>`, `<type_traits>` |
| `include/eph/containers/bounded_queue.hpp` | SPSC 有界无锁队列（队满阻塞/失败） | `BoundedQueue<T, Capacity>` | `concepts.hpp`, `eph-utils` |
| `include/eph/containers/evicting_queue.hpp` | SPSC 可丢弃队列（队满覆盖旧数据） | `EvictingQueue<T, Capacity>`, `EvictingQueue<T, 1>` 特化 | `concepts.hpp`, `eph-utils` |
| `include/eph/containers/bounded_queue_bytes.hpp` | BoundedQueue 的字节流适配器 | `BoundedQueueBytes<MaxDataSize, Capacity>` | `bounded_queue.hpp` |
| `include/eph/containers/evicting_queue_bytes.hpp` | EvictingQueue 的字节流适配器 | `EvictingQueueBytes<MaxDataSize, Capacity>` | `evicting_queue.hpp` |

## 4. 数据流

### BoundedQueue 数据流

生产者写入元素到环形缓冲区，消费者按 FIFO 顺序读取。队列满时写入失败（try 系列）或自旋等待（阻塞系列）。

```
Producer Thread                    Consumer Thread
     |                                  |
     v                                  v
 try_push/produce                  try_pop/consume
     |                                  |
     v                                  v
 check shadow_head_              check shadow_tail_
 (local cached head)             (local cached tail)
     |                                  |
     |  [cache miss? reload]            |  [cache miss? reload]
     v                                  v
 write buffer_[tail & mask]      read buffer_[head & mask]
     |                                  |
     v                                  v
 tail_.store(release)            head_.store(release)
```

### EvictingQueue 数据流

生产者 wait-free 写入，队满时覆盖旧数据。消费者通过 SeqLock 乐观读取最新数据，读取失败（被写入者打断）时重试。

```
Producer Thread                    Consumer Thread
     |                                  |
     v                                  v
 produce(writer_func)            try_consume_latest(visitor)
     |                                  |
     v                                  v
 slot.seq = odd (locked)         load global_index_ (acquire)
     |                                  |
     v                                  v
 fence(release)                  load slot.seq (acquire)
     |                                  |
     v                                  v
 write slot.data                 if odd -> return false
     |                                  |
     v                                  v
 fence(release)                  read slot.data
     |                                  |
     v                                  v
 slot.seq = even (unlocked)      fence(acquire)
     |                                  |
     v                                  v
 global_index_.store(release)    re-check slot.seq
                                 seq1 == seq2 ? success : retry
```

## 5. 关键组件

### 5.1 BoundedQueue<T, Capacity>

- **文件**: `include/eph/containers/bounded_queue.hpp`
- **用途**: SPSC 有界无锁环形队列，元素按 FIFO 顺序消费，队满时写入失败或阻塞
- **内存布局**: WriterLine (cache line) + ReaderLine (cache line) + buffer_[Capacity]
- **关键接口**:
  ```cpp
  bool try_push(U&& data) noexcept;
  bool try_produce(F&& writer_func) noexcept;     // 零拷贝写入
  bool try_push_n(std::span<const T> data) noexcept; // 批量 all-or-nothing
  bool try_pop(T& out) noexcept;
  bool try_consume(F&& visitor) noexcept;          // 零拷贝读取
  size_t try_pop_n(std::span<T> out) noexcept;     // 批量尽力而为
  // 阻塞版: push/produce/pop/consume (cpu_relax 自旋)
  // 超时版: try_*_for (deadline 自旋)
  ```
- **设计要点**: 影子索引（shadow_head_/shadow_tail_）缓存对端指针，大幅减少跨核 cache line 读取。批量操作单次 atomic store 发布，摊销原子操作开销。

### 5.2 EvictingQueue<T, Capacity>

- **文件**: `include/eph/containers/evicting_queue.hpp`
- **用途**: SPSC 可丢弃队列，Writer wait-free（永不阻塞，满时覆盖），Reader lock-free（乐观 SeqLock 读取最新数据）
- **内存布局**: global_index_ (cache line) + WriterLine (cache line) + ReaderLine (cache line) + Slot[Capacity]（每个 Slot 含 seq 原子序列号 + data）
- **关键接口**:
  ```cpp
  void produce(F&& writer_func) noexcept;          // wait-free 写入
  void push(U&& val) noexcept;
  bool try_consume_latest(F&& visitor) noexcept;    // 乐观读取
  T pop_latest() noexcept;                          // 阻塞读取
  ```
- **设计要点**: seq 低 1 位为 lock flag，高 63 位为 global index。写入时先锁定 slot（seq 置奇），写完解锁（seq 置偶）。读取时双重检查 seq 一致性验证数据完整性。

### 5.3 EvictingQueue<T, 1> (SeqLock 特化)

- **文件**: `include/eph/containers/evicting_queue.hpp`（同文件，模板偏特化）
- **用途**: 单槽位 SeqLock，最简化的"总是保留最新值"语义
- **内存布局**: seq_ (cache line) + last_seq_ (cache line) + data_ (cache line)
- **设计要点**: seq 偶数 = 空闲，奇数 = 写入中。每次写入 seq += 2。比通用版更紧凑，适用于"最新行情/状态"等场景。

### 5.4 BoundedQueueBytes<MaxDataSize, Capacity>

- **文件**: `include/eph/containers/bounded_queue_bytes.hpp`
- **用途**: BoundedQueue 的字节流适配器，将变长字节数据封装为定长 DataWrap 存入队列
- **DataWrap 结构**: `{ uint64_t ts; uint32_t len; std::array<uint8_t, MaxDataSize> data; }`
- **关键接口**: `try_push/try_pop/try_consume`（纯字节流 + 可选时间戳）、`try_push_n/try_consume_n`（批量）
- **设计要点**: 底层委托 `BoundedQueue<DataWrap, Capacity>`。读取时 `safe_len = min(msg.len, MaxDataSize)` 防止脏数据越界。

### 5.5 EvictingQueueBytes<MaxDataSize, Capacity>

- **文件**: `include/eph/containers/evicting_queue_bytes.hpp`
- **用途**: EvictingQueue 的字节流适配器，附加消息 ID 用于丢包统计
- **DataWrap 结构**: `{ uint64_t id; uint64_t ts; uint32_t len; std::array<uint8_t, MaxDataSize> data; }`
- **关键接口**: `try_consume_latest_wts(visitor)` 回调签名 `void(span<const uint8_t>, uint64_t ts, uint32_t discarded)`
- **设计要点**: Writer 端维护 push_count_（cache line 隔离），Reader 端维护 last_pop_id_，通过 `id - last_pop_id_ - 1` 计算被覆盖丢弃的消息数。

### 5.6 TrivialData concept

- **文件**: `include/eph/containers/concepts.hpp`
- **定义**: `std::is_trivially_copyable_v<T> && std::default_initializable<T>`
- **用途**: 所有队列的类型约束，确保元素可安全 memcpy 且有默认构造函数

## 6. 入口点与 API

| 入口 | 类型 | 说明 |
|------|------|------|
| `#include "eph/containers.hpp"` | umbrella header | 包含所有容器 |
| `BoundedQueue<T, Cap>` | 类模板 | SPSC FIFO 有界无锁队列 |
| `EvictingQueue<T, Cap>` | 类模板 | SPSC 可丢弃队列 (SeqLock) |
| `EvictingQueue<T, 1>` | 偏特化 | 单槽位 SeqLock |
| `BoundedQueueBytes<MaxSize, Cap>` | 类模板 | 字节流有界队列 |
| `EvictingQueueBytes<MaxSize, Cap>` | 类模板 | 字节流可丢弃队列 |
| `TrivialData<T>` | concept | 类型约束 |

## 7. 依赖

### 内部模块依赖图

```
containers.hpp
  +-- bounded_queue.hpp ------+-- concepts.hpp
  +-- evicting_queue.hpp -----+-- concepts.hpp
  +-- bounded_queue_bytes.hpp -- bounded_queue.hpp
  +-- evicting_queue_bytes.hpp - evicting_queue.hpp
```

### 外部依赖

| 包 | 模块 | 用途 |
|----|------|------|
| `eph-utils` | `eph/utils/alignment.hpp` | `CACHE_LINE_SIZE` (64), `Align<T>` cache line 对齐常量 |
| `eph-utils` | `eph/utils/cpu.hpp` | `cpu_relax()` 自旋等待指令 (x86 PAUSE / ARM64 YIELD) |
| C++ 标准库 | `<atomic>`, `<array>`, `<span>`, `<optional>`, `<chrono>`, `<bit>`, `<concepts>` 等 | 原子操作、容器、时间、类型约束 |

无第三方外部依赖（eph-utils 中的 spdlog 仅在 cpu.hpp 的拓扑检测函数中使用，容器本身不引入日志）。

## 8. 测试

本子项目目录下未发现独立测试文件。测试可能位于上层 ephemeral 项目的统一测试目录中。

| 场景 | 覆盖状态 | 说明 |
|------|----------|------|
| BoundedQueue SPSC 基本读写 | 未知 | 需检查上层测试目录 |
| BoundedQueue 队满/队空边界 | 未知 | try_push 返回 false / try_pop 返回 nullopt |
| BoundedQueue 批量读写 | 未知 | try_push_n / try_pop_n |
| EvictingQueue 覆盖旧数据 | 未知 | Writer 连续写入超过 Capacity |
| EvictingQueue SeqLock 脏读重试 | 未知 | Reader 读取期间被 Writer 打断 |
| EvictingQueue<T,1> 单槽特化 | 未知 | SeqLock 最简场景 |
| Bytes 适配器丢包统计 | 未知 | EvictingQueueBytes discarded 计数 |
| 超时接口 | 未知 | try_*_for 系列 |

### 模板复杂度说明

- 所有容器均为类模板，Capacity 要求 2 的幂（编译期 `static_assert` 检查）
- 大量使用 C++20/23 特性：concepts 约束、`requires` 子句、`std::invocable`、`std::span`、`[[nodiscard]]`、`[[unlikely]]`
- 零拷贝接口通过 Visitor 模式（回调 lambda）实现，避免模板返回类型问题
- 无 unsafe/FFI 边界，纯 C++ 实现；使用内联汇编仅限 `cpu_relax()` 中的 PAUSE/YIELD 指令
