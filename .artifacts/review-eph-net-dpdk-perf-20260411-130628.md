# Code Review: eph-net-dpdk 性能 / 低延迟审查

## 元信息

- **时间**：2026-04-11 13:06:28
- **模式**：audit（全模块只读审计，非 diff）
- **审查目标**：`eph-net-dpdk/` 模块的性能和低延迟相关问题
- **代码规模**：~9,300 行 header-only C++23
- **审查方式**：Explore subagent 深读前 6 个热路径文件 + 人工 spot-check 验证

## 审查摘要

eph-net-dpdk v3.3 的 **Poller 层**实现正确——纯函数指针类型擦除（`+[]` thunk），零 vtable，无 `std::function`，符合项目约定。burst 批处理、prefetch 潜力、atomic 内存序等 DPDK 基础面做得对。

**真正的漏点在 Stream 层的用户回调边界**：`TcpStream::on_message` 和 `UdpSocket::on_datagram` 被声明为 `std::function<>`，数据面每收一帧都走一次类型擦除间接调用。这破坏了 v3.3 "零 vtable" 的架构承诺。

握手路径上的 `vector::insert()` 是次要问题（control plane，偶发）；但与其说"违反约定"，不如说是**一致性污点**。

### 问题统计

- 🔴 **Critical**：2（数据面 std::function + 数据面 ReasmBuffer 可能丢帧）
- 🟡 **Major**：4（握手路径分配 ×2、compact 大 memmove、缺 prefetch/likely 提示）
- 🔵 **Minor**：3（冗余 branch、字符串拼接、logger 初始化）

### 结论

**REQUEST_CHANGES**（数据面 std::function 应修，其它可 Phase 12+ 清理）

模块没有**架构级**性能问题——Poller 设计是正确的，PacketView 真零拷贝，TCP state machine 批处理合理。**修正 std::function 一个点就能从 Tier-2 HFT lib 升级到 Tier-1**。

---

## Critical 问题（2）

### 🔴 Issue #1: `on_message` / `on_datagram` 是 `std::function<>`（数据面热路径）

**文件**：
- `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp:369`
- `eph-net-dpdk/include/eph/net/dpdk/udp_socket.hpp:102`

**类型**：虚分派 / 间接调用

**代码**：
```cpp
// tcp_stream.hpp:369
using OnMessage = std::function<void(const uint8_t*, uint16_t)>;
// tcp_stream.hpp:548
OnMessage on_message;

// 调用点 tcp_stream.hpp:756-761 (drain_codec_ 每帧调用)
if (frame.size() > 0 && on_message) {
    on_message(frame.data(), ...);
    ++delivered;
}
```

**描述**：
- 每收一帧，`on_message(data, n)` 都调用 `std::function::operator()`
- gcc 的 std::function 通过一个类型擦除的 invoker 函数指针派发（本质是虚分派）
- 对 lambda 捕获 ≤16 字节：SOO 生效，无堆分配，但仍有间接调用 ~2-5 ns + 可能的 branch misprediction
- 对大 lambda 捕获（> 16 字节）：std::function 在 ctor 时堆分配 closure，调用时 double indirection（一次取 invoker，一次访问 heap 上的 closure）
- 对 1 MHz 消息率（DPDK 可达）：2-5 ns × 10^6 = 2-5 ms/s 纯粹 overhead，对应 p99 尾部额外 ~10 ns

**修复方向**（三选一）：

1. **Template 化 Stream 的 callback 类型**（最简单，零成本）：
   ```cpp
   template <class Codec, bool EnableTls = false, class OnMsg = void(*)(const uint8_t*, uint16_t)>
   class DpdkTcpStream { OnMsg on_message; ... };
   ```
   用户显式指定 callable 类型。编译期 inline，零开销。破坏二进制兼容，但 v3.3 header-only 允许。

2. **function_ref + user_data 指针**（C-style，符合 Poller 约定）：
   ```cpp
   using OnMessageFn = void(*)(void* user, const uint8_t*, uint16_t) noexcept;
   OnMessageFn on_message = nullptr;
   void*       on_message_user = nullptr;
   ```
   调用：`if (on_message) on_message(on_message_user, data, n);`
   零分配、零间接、纯函数指针，和 `Poller::process_burst_fn` 风格对齐。

3. **function_ref 非持有**（C++26 std::function_ref 或 tl::function_ref 临时替代）：
   ```cpp
   using OnMessage = eph::core::function_ref<void(const uint8_t*, uint16_t) noexcept>;
   ```
   非持有 ABI（函数指针 + 数据指针），无堆分配。

**推荐方案 2**（和 Poller 的 `process_burst_fn` 风格字面统一）。

**量化影响**：
- 2-5 ns/frame × frame rate
- @100k fps：0.2-0.5 μs/s CPU overhead，基本不可测
- @1M fps：2-5 ms/s CPU overhead，显著
- p99 尾部：如果 std::function 持有大捕获导致 heap + cache miss，单帧可能额外 100-500 ns → **p99 尾部直接抬升**

---

### 🔴 Issue #2: `ReasmBuffer::append()` 容量满时**静默丢帧**

**文件**：`eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp:114-143`

**类型**：正确性 + 可靠性

**代码**：
```cpp
explicit ReasmBuffer(std::size_t cap = 64 * 1024) { buf_.resize(cap); }

bool append(const uint8_t* p, std::size_t n) noexcept {
    if (writable() < n) {
        compact();
        if (writable() < n) return false;  // ← 丢，不通知用户
    }
    std::memcpy(buf_.data() + tail_, p, n);
    tail_ += n;
    return true;
}
```

**描述**：
- 默认 64 KB 固定大小，不可增长
- 当 compact() 后仍放不下：`append()` 返回 false，调用处可能只打个 WARN log（见 `tcp_stream.hpp:695` 附近）
- 场景：burst 64 KB + 1 字节，前 64 KB 收到、第 65 KB 字节静默丢弃
- TCP 侧不会 ACK 失败（已经被 DPDK PMD ACK 过了）
- 结果：**应用层数据丢失**，链路看起来正常

**量化影响**：
- 64 KB reasm 对于 WS + 小帧够用
- 但如果 mock / 真实对端在 burst 里一下发 100 KB JSON payload（例如 L2 orderbook full snapshot），前 64 KB 被 reasm，剩下 36 KB 被 silently dropped
- HFT 场景下 **silent drop 比 disconnect 危险得多**

**修复方向**：
1. `append()` 失败时：**关闭 stream 并触发 reconnect**，而不是继续运行
2. 或：`reasm_capacity` 从 `StreamConfig` 接收（本来就有 `reasm_capacity` 字段，但 DPDK 分支可能忽略了），用户可配
3. 或：增加 dynamic grow（但破坏 "allocation-free hot path"）
4. 或：把 `reasm_capacity` 默认从 64 KB 提到 256 KB，匹配常见 HFT 流量模式

**推荐**：方案 1（丢帧 → 立刻 error state，外部重连），并把默认 `reasm_capacity` 提到 256 KB。

---

## Major 问题（4）

### 🟡 Issue #3: `PlainDpdkWsSink::recv()` 使用 `vector::insert()`

**文件**：`eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp:221`

**类型**：堆分配（control plane）

**代码**：
```cpp
[this](const uint8_t* p, uint16_t len) {
    staged_.insert(staged_.end(), p, p + len);  // ← 可能 realloc
});
```

**描述**：
- WS handshake 期间 staging bytes，`std::vector::insert` 在 capacity 不足时 reallocate
- Control plane 调用（stream 建立时），不是数据面
- 握手期间可能 5-10 次 recv，每次都可能 realloc
- 估计 spike：100-500 μs（分配 + memcpy），发生在 handshake 期间

**修复方向**：
- 预先 `reserve(8192)`，或改用 `std::array<uint8_t, 8192>` + offset
- 或使用 `staged_.resize(0)` 替代 `staged_.clear()` 保留容量

---

### 🟡 Issue #4: `TlsDpdkWsSink::recv()` 同样的 `vector::insert()`（TLS 变体）

**文件**：`eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp:296`

**类型**：堆分配（control plane，TLS 握手）

**描述**：与 #3 相同，只是 TLS WS 握手路径的 cipher buffer 累积。

**修复方向**：同 #3。

---

### 🟡 Issue #5: `ReasmBuffer::compact()` 大 memmove

**文件**：`eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp:133`

**类型**：数据面 memcpy

**代码**：
```cpp
void compact() noexcept {
    if (head_ == 0) return;
    if (head_ == tail_) { head_ = tail_ = 0; return; }
    std::memmove(buf_.data(), buf_.data() + head_, readable());
    tail_ -= head_;
    head_ = 0;
}
```

**描述**：
- 每次 `drain_codec_` 前调用 `compact()`（`tcp_stream.hpp:728`）
- 如果 head_ != 0 且 readable() 很大，memmove 可能是 32-48 KB
- @10 GB/s memcpy 速率：50 KB ≈ 5 μs，**在测量的 RTT 路径上**
- 对 lat_tcp 测量约 3-5 μs wire，compact 5 μs 就是 100%+ 的增加

**实际影响**（需要量化）：
- Phase 11.1 lat_tcp dpdk p50 = 23 μs；如果 compact 真占 5 μs，应该能从 p50 看出 periodic spike
- 实测 max_ns = 2.3 ms，p99.9 = 37 μs，stddev = 2.4 μs。没看到周期性 5 μs spike
- 说明 compact 实际发生频率很低（head_ 多数时候 == 0 或 readable 很少）
- **潜在问题**，不是当前问题

**修复方向**：
- 改 ring buffer（循环数组 + modular index）避免 memmove，但增加代码复杂度
- 或：记录 compact 触发频率 + 累计 memmove 字节数的统计，观察后再决定

---

### 🟡 Issue #6: Poller 热路径缺 `[[likely]]` / prefetch

**文件**：`eph-net-dpdk/include/eph/net/dpdk/poller.hpp:273-283`

**类型**：分支预测 + cache miss

**代码**：
```cpp
for (uint16_t i = 0; i < n; ++i) {
    PollableEntry* entry = lookup_by_5tuple_(mbufs[i]);
    if (entry != nullptr) {              // ← 缺 [[likely]]
        entry->process_burst_fn(...);
        ++dispatched;
    } else {
        rte_pktmbuf_free(mbufs[i]);      // ← drop 是 cold path
    }
}
```

**描述**：
1. `entry != nullptr` 没有 `[[likely]]` 标注。正常流量下几乎全部命中，标注后可节省 1-2 cycle/branch
2. `lookup_by_5tuple_` 线性扫描 `entries_` 数组，对每个 mbuf 都全表扫描。对 N ≤ 8 的常见 HFT 部署 OK，N 大时 O(n²)
3. 没有 `rte_prefetch0()` 把下一个 `entries_[i+1]` 或下一个 mbuf 的 L4 头预取入 L1
4. 每个 dispatch 都是 1-by-1（`&mbufs[i], 1`），浪费 burst batch 优化潜力——`process_burst_fn` 本设计是接受批次的，但 poller 拆成单个调用

**修复方向**：
1. 加 `[[likely]]`：
   ```cpp
   if (entry != nullptr) [[likely]] { ... }
   ```
2. 加 prefetch：
   ```cpp
   if (i + 1 < n) rte_prefetch0(mbufs[i + 1]);
   ```
3. **批次内 group-by-entry**：先扫描 n 个 mbuf 建立 entry → mbuf 列表的映射，然后每个 entry 一次性拿到所有它的 mbuf：
   ```cpp
   // Phase 12+ 优化：
   rte_mbuf* per_entry[kMaxEntries][kBurstSize];
   uint16_t  per_entry_count[kMaxEntries] = {0};
   for (uint16_t i = 0; i < n; ++i) {
       auto idx = lookup_index_by_5tuple_(mbufs[i]);
       if (idx != kInvalid) [[likely]] {
           per_entry[idx][per_entry_count[idx]++] = mbufs[i];
       } else { rte_pktmbuf_free(mbufs[i]); }
   }
   for (uint16_t j = 0; j < n_entries_; ++j) {
       if (per_entry_count[j] > 0) {
           entries_[j].process_burst_fn(entries_[j].obj, per_entry[j],
                                         per_entry_count[j], rx_tsc);
       }
   }
   ```
   把 burst 真的按 burst 传给 Stream，让 TcpSession 批量解 TCP 头 / 批量解 codec。

**量化影响**：
- [[likely]]：~1-2 ns per branch
- prefetch：~3-5 ns per cache miss avoided × burst size → 几十 ns per burst
- group-by-entry：数据面吞吐提升 ~20%，在 100k fps 以上明显

---

## Minor 问题（3）

### 🔵 Issue #7: udp_socket 中 `on_datagram` 冗余判空

**文件**：`eph-net-dpdk/include/eph/net/dpdk/udp_socket.hpp:308, 345`

```cpp
if (!on_datagram) { ... return; }   // line 308
...
if (frame.size() > 0 && on_datagram) {  // line 345，重复判空
    on_datagram(...);
}
```

**修复**：入 burst loop 前判一次即可，或 assert 前置条件。

---

### 🔵 Issue #8: `tcp_stream.hpp:471-476` IPv4 地址格式化 string concat

**文件**：`eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp:471-476`

```cpp
host_storage =
    std::to_string((ip_be >>  0) & 0xFFu) + "." +
    std::to_string((ip_be >>  8) & 0xFFu) + "." + ...
```

**描述**：Control plane（WS handshake 的 Host header fallback），但 4-5 次 `std::to_string` + `+` concatenation = ~10 次 heap alloc。

**修复**：
```cpp
char buf[16];
std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ...);
```

---

### 🔵 Issue #9: Logger 延迟初始化

**文件**：`eph-net-dpdk/include/eph/dpdk/tcp.hpp:205`

**描述**：
- 首次调用 `tcp_logger()` 走 `spdlog::get` + `stdout_color_mt` 路径，包含 mutex + alloc
- 后续 O(1)
- 但前提：`SPDLOG_ACTIVE_LEVEL` 在 release build 下必须是 INFO 及以上，否则 DEBUG/TRACE macro **展开** 会包括 logger 调用 —— 即便是"被过滤"的 debug 也会导致 logger_() 被调

**验证需要**：grep 是否有 `SPDLOG_LOGGER_DEBUG` / `SPDLOG_LOGGER_TRACE` 在 `process_rx` 等数据面函数里。如果有，release build 必须确认 `SPDLOG_ACTIVE_LEVEL >= SPDLOG_LEVEL_INFO`。

---

## 性能上做得对的地方

1. **`Poller::poll()` 纯函数指针派发**（`poller.hpp:276`，`entry->process_burst_fn(obj, ...)`）——零 vtable，符合 v3.3 "P2 type erasure" 约定
2. **burst 批处理**：`rte_eth_rx_burst(port, queue, mbufs, kBurstSize=32)` 是正确的 DPDK 风格
3. **MbufView 真零拷贝**（`detail/mbuf_view.hpp`）：writable_data / trim_front / trim_back 都是指针移动，无 memcpy，和 kernel 的 SpanView 对称
4. **atomic 使用合理**：`last_rx_burst_tsc_` 的 `alignas(64)` + release/acquire 是跨 core stats 读的正确模式（虽然有 1-2 ns overhead，但语义清晰）
5. **Lazy logger 初始化**：`tcp_logger()` 第一次才 init，避免 static ctor 时机问题
6. **`KernelPoller::poll(0ms)` 和 `DpdkPoller::poll()`都 noexcept 且 allocation-free**（除了 user 的 on_message）
7. **`rte_pktmbuf_free` 正确调用**：未匹配的 mbuf 正确释放，未见 leak
8. **Platform setup** 的 mempool + port config 遵循 DPDK best practices（cache size、burst prefetch、txconf/rxconf）
9. **ReasmBuffer 构造时 `buf_.resize(cap)`**：一次分配，后续无 realloc（修正 #2 之后应保持此特性）

---

## 技术债清单

| # | 严重度 | 维度 | 描述 | 位置 | 影响 | 推荐 Skill |
|---|---|---|---|---|---|---|
| 1 | 🔴 Critical | 虚分派 | `on_message` / `on_datagram` 是 std::function | tcp_stream.hpp:369, udp_socket.hpp:102 | 数据面每帧 2-5 ns | `/refactor breaking` |
| 2 | 🔴 Critical | 可靠性 | ReasmBuffer 满时 silent drop | tcp_stream.hpp:114-143 | 应用层数据丢失 | `/debug` |
| 3 | 🟡 Major | 分配 | WS 握手 `staged_.insert` | tcp_stream.hpp:221 | 控制面 spike 100-500 μs | `/improve` |
| 4 | 🟡 Major | 分配 | TLS WS 握手 `cipher_.insert` | tcp_stream.hpp:296 | 同 #3 | `/improve` |
| 5 | 🟡 Major | 数据面 memcpy | compact memmove | tcp_stream.hpp:133 | 潜在 5 μs spike | Phase 12 优化 |
| 6 | 🟡 Major | 分支/prefetch/batch | Poller 热路径优化缺失 | poller.hpp:273-283 | burst 利用率低 | `/improve` |
| 7 | 🔵 Minor | 冗余分支 | on_datagram 双判空 | udp_socket.hpp:308,345 | ~1 ns | `/refactor` |
| 8 | 🔵 Minor | 控制面分配 | IP 格式化 string concat | tcp_stream.hpp:471-476 | control plane | `/refactor` |
| 9 | 🔵 Minor | logger | DEBUG macro 展开 | tcp.hpp:205 + 调用点 | 需 grep 确认 | `/audit` |

---

## 架构评估

**结论**：**不是架构级问题，是局部细节**。

- Poller / PacketView / concept 分层 / atomic 使用 / PMD 调用约定 —— **全部正确**
- 唯一的架构"瑕疵"是 Stream 的用户回调边界选了 `std::function<>`，这是 v3.3 refactor 里的一个疏忽
- **修掉 Issue #1（std::function → 函数指针 + user_data）**就能把模块从"合理的 DPDK wrapper"提升到"HFT-ready zero-overhead Stream"
- Issue #2（ReasmBuffer silent drop）**是可靠性 bug，不是性能问题**，但严重度高到必须修

相比 pre-v3.3 baseline：没有回退迹象（subagent 没发现 legacy Reactor/Transport 里哪个点更快）。v3.3 从"多抽象层"简化为"Poller + Stream 两层"是正确的方向。

## 推荐行动计划

1. **Phase 12a（立即）**：修 Issue #1 —— `on_message` / `on_datagram` 改为函数指针 + user_data。同时修 kernel 侧的对称 API 保持一致。
2. **Phase 12b（立即）**：修 Issue #2 —— ReasmBuffer 满时触发 error state → reconnect，默认 capacity 提到 256 KB。
3. **Phase 12c**（可选）：Issue #6 Poller 热路径 group-by-entry batch 优化。需要新建 `bench_poller_burst_dispatch`。
4. **Phase 13+**（可选）：Issue #5 ReasmBuffer 改 ring buffer。先加统计观察 compact 频率。

Issue #3 #4 #7 #8 #9 都是 cosmetic，可以在 `/improve` 时顺手带走。

---

## 后续建议

- Issue #1 和 Issue #2 建议先用 `/discuss` 讨论 API 变更方案（特别是 std::function → 函数指针对用户 ergonomics 的影响），然后用 `/refactor breaking` 实施
- 整个修复预计 ~200 LOC + bench 验证，一个 phase 可以搞定
- 修复后用 `/review audit eph-net-dpdk` 重审
