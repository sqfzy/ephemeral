# Plan: DPDK Source Port Selection Helper

> 为 `eph-net-dpdk` 补齐 `DpdkPoller` 的 4-tuple 唯一性检查，并在 Poller 上提供一个 `pick_src_port` 建议性查询方法，服务于作为 TCP client 时的动态源端口选择。

创建时间：2026-04-14
状态：已确认（auto 模式 → 2026-04-14 简化修订）
讨论依据：`.artifacts/discuss-dpdk-tcp-src-port-allocation-20260414-025000.md`

---

## 修订说明（2026-04-14）

初版本 plan 设计了独立的 `SourcePortPool` + `SourcePortLease` + `DpdkTcpStream::create(cfg, lease)` 重载（~730 LOC）。在进入实施前的审视中发现：**阶段 1 的 `DpdkPoller::add` 4-tuple 唯一性检查一旦落地，就已经是权威的冲突判定器**；一个独立的 `SourcePortPool` 位图会成为该权威的冗余副本，引入：

- 两个数据结构需要同步（pool 位图 ↔ poller entries_）
- lease 生命周期必须与 pool 解耦但又被 stream 持有的所有权谜题
- 2MSL grace 窗口在 28k 端口范围下对 0.0036% 概率事件的过度保护
- 不存在的场景下的并发锁（Poller 本身不是 MT-safe，典型单 driver 线程用法）

修订方案：**不引入任何新类型**。在 `DpdkPoller` 上加一个 `pick_src_port()` 查询方法，扫描 `entries_` 的现状，返回一个不与任何已注册 4-tuple 冲突的源端口。用户用法简单串行：

```cpp
auto port = poller->pick_src_port(src_ip, dst_ip, 443).value();
cfg.legacy.tuple.src_port = port;
auto stream = DpdkTcpStream::create(std::move(cfg)).value();
poller->add(stream.get()).value();   // ← 权威终点检查
```

`Poller::add` 是 single source of truth；`pick_src_port` 只是便利层。失败自然经过 `add` 的错误路径。

**对讨论 artifact 中各角色的映射**：

- **R3 flow-director preregister 场景**：不受影响 —— 继续走老 `create(cfg)` + 硬编码 `src_port`，完全不经过 `pick_src_port`
- **R14 显式所有权建模**：更严格 —— 只有一个权威（Poller），没有 pool 与 Poller 两个数据源同步的风险
- **R2 需求密度 <10 不值得抽象**：修正后没有新类型、没有新 namespace entity、只是在已有 Poller 上加一个查询方法，复杂度预算大幅减少
- **R1 硬条件 (a) Poller 4-tuple 检查**：保留为阶段 1
- **R1 硬条件 (b) lease 失败路径集成测试**：不再适用（没有 lease）
- **R1 硬条件 (c) 位图 + 2MSL grace**：不再适用；用 28k 随机范围下的撞车概率 0.0036% 作为替代论据

**规模对比**：

| 方案 | 实现 LOC | 测试 LOC | 新类型数 | Commit 数 |
|------|---------|---------|---------|----------|
| 初版（SourcePortPool） | ~230 | ~500 | 3 | 6 |
| 修订版（pick_src_port） | ~60 | ~160 | 0 | 3 |

---

## 定位与边界

**目标**：以最小代价补齐 DPDK 端作为 TCP client 时的源端口选择能力，同时消除 `DpdkPoller::add` 已存在的 4-tuple 唯一性空洞。

**用户**：

- 现有单连接 / flow-director 硬编码 DPDK 用户：**完全不受影响**，老 `create(cfg)` 接口和老用法零改动
- 未来多 venue / 多 session 用户：在 `poller->add` 之前先调一次 `poller->pick_src_port` 取端口

**In scope**：

- 前置修复：`DpdkPoller::add()` 新增 4-tuple 唯一性检查（独立于本修订的 helper，是一个现存 bug 的修复）
- 新增 `DpdkPoller::pick_src_port()` 成员方法
- Poller 单元测试扩展：4-tuple 冲突拒绝、`pick_src_port` 语义覆盖

**Out of scope**（严格遵守讨论收敛）：

- `TcpConfig::validate()` 的 `src_port != 0` 硬约束**不变** —— `pick_src_port` 在外部返回端口，用户写入 cfg 后走老 `create(cfg)` 路径，validate 照常执行
- `DpdkTcpStream::create` **不新增重载**、签名完全不变
- `eph::net::kernel::*`（kernel 有原生 bind(0)，不引入对称 helper）
- Bench 客户端（`lat_*_dpdk`）迁移 —— 保持硬编码以维持可复现
- `DpdkUdpSocket` 的 helper —— UDP 语义上可以共享同一 helper，但本 plan 不涵盖，列入 Out of scope，需要时再加
- 2MSL grace 窗口 / 端口回收延迟 —— 28k 范围下随机撞车概率 0.0036%，不值得实现成本
- RAII lease / 独立 Pool 类 / 位图 / grace deque —— 全部删除

---

## 技术选型

| 类别 | 选择 | 理由 |
|------|------|------|
| 实现形态 | 在 `eph-net-dpdk/include/eph/net/dpdk/poller.hpp` 中扩展 `DpdkPoller` 类 | 不新增 header，不新增 namespace 实体 |
| 查询算法 | 随机起点 + 线性探测整个 range | Range ≤ 28k，每次扫描最坏 O(range × n_entries)；`n_entries_` 默认 <= `kMaxConn`，成本可忽略 |
| 随机源 | `getrandom(2)` | 与 `eph::dpdk::TcpSession` ISN 生成一致（见 tcp.hpp 头部 "getrandom(2) for ISN generation" 注释） |
| 错误通道 | `std::expected<uint16_t, core::ErrorInfo>` | 与 v3.3 错误契约对齐 |
| 线程安全 | **无锁**，明确文档为 "建议性查询" | Poller 本身非 MT-safe，典型单 driver 线程用法；加锁会引入假设外的并发模型 |
| 日志 | 复用 `detail::poller_logger()` | `pick_src_port` 是 Poller 的成员，用自己的 logger |
| 测试框架 | gtest，复用现有 `test_dpdk_poller.cpp` 目标 | 不新建测试文件；所有新测试用例追加到既有文件 |

---

## 架构设计

### 修改文件清单

| 文件 | 动作 | 职责 |
|------|------|------|
| `eph-net-dpdk/include/eph/net/dpdk/poller.hpp` | 修改 | (1) `add()` 扩展 4-tuple 唯一性检查 (2) 新增 `pick_src_port()` 成员方法 |
| `eph-net-dpdk/tests/test_dpdk_poller.cpp` | 修改 | 新增 4-tuple 冲突拒绝 + `pick_src_port` 语义测试 |
| `eph-net-dpdk/CHANGELOG.md` | 修改 | 记录两项变更 |

**不修改**：

- `eph-net-dpdk/include/eph/dpdk/tcp.hpp`（`TcpConfig::validate()` 的 src_port 约束保留不变）
- `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp`（`DpdkTcpStream` 签名不变）
- `eph-net-dpdk/include/eph/net/dpdk/config.hpp`（`StreamConfig` 不新增字段）
- 任何 kernel backend 文件
- 任何 bench/example 文件

### 核心接口

```cpp
template <class P>
class DpdkPoller {
public:
    // ... 既有 API 完全不变 ...

    /// @brief Suggest an unused source port for a new TCP client connection.
    ///
    /// Scans the currently registered Pollables and returns a port in
    /// `[range_begin, range_end]` such that the 4-tuple
    /// `(src_ip, dst_ip, dst_port, result)` does not conflict with any
    /// existing entry.
    ///
    /// Selection policy:
    ///   - If `preferred != 0` and is within range and not in use, return it
    ///     directly (hard preference path, cheap).
    ///   - Otherwise, `getrandom(2)` picks a random starting point in the
    ///     range and linear-probes forward modulo the range until the first
    ///     non-conflicting port is found.
    ///   - If all ports in the range are occupied by the given
    ///     (src_ip, dst_ip, dst_port), returns `OutOfMemory`.
    ///
    /// Thread safety: advisory query only. The returned port can become
    /// stale the instant another thread modifies the Poller. `DpdkPoller`
    /// itself is not MT-safe; typical usage is single driver thread.
    ///
    /// Usage:
    ///     auto port = poller->pick_src_port(src_ip, dst_ip, 443).value();
    ///     cfg.legacy.tuple.src_port = port;
    ///     auto stream = DpdkTcpStream::create(std::move(cfg)).value();
    ///     auto add_r = poller->add(stream.get());  // authoritative check
    ///
    /// On `add` failure (race or stale data), the caller may retry by
    /// calling `pick_src_port` again — it reflects the latest entries_.
    ///
    /// @param src_ip       Source IPv4 (host order) — must match cfg.tuple.src_ip
    /// @param dst_ip       Destination IPv4 (host order)
    /// @param dst_port     Destination port (host order)
    /// @param range_begin  Inclusive lower bound (default 32768, Linux ephemeral)
    /// @param range_end    Inclusive upper bound (default 60999, Linux ephemeral)
    /// @param preferred    Optional soft preference (0 = no preference)
    ///
    /// @return The selected port, or an `ErrorInfo` with:
    ///         - `InvalidConfig` if the range is inverted / below 1024 / preferred
    ///           is non-zero but out of range
    ///         - `OutOfMemory` if every port in the range is in use by the
    ///           given (src_ip, dst_ip, dst_port)
    [[nodiscard]] std::expected<uint16_t, core::ErrorInfo>
    pick_src_port(uint32_t src_ip,
                  uint32_t dst_ip,
                  uint16_t dst_port,
                  uint16_t range_begin = 32768,
                  uint16_t range_end   = 60999,
                  uint16_t preferred   = 0) const noexcept;
};
```

### `DpdkPoller::add()` 4-tuple 唯一性检查（前置修复）

修改 `eph-net-dpdk/include/eph/net/dpdk/poller.hpp` 的 `add()` 函数（当前第 180-230 行）：

**修改前**（现状，只查 obj 重复）：
```cpp
for (std::size_t i = 0; i < n_entries_; ++i) {
    if (entries_[i].obj == static_cast<void*>(obj)) {
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "DpdkPoller::add: already registered"});
    }
}
// ... 然后才调用 tuple_fn 取 4-tuple ...
```

**修改后**（4-tuple 合并进同一次线性扫描）：
```cpp
// 先取 4-tuple（before 扫描），因为扫描需要比对
uint32_t new_src_ip = 0, new_dst_ip = 0;
uint16_t new_src_port = 0, new_dst_port = 0;
obj->tuple_for_poller_(&new_src_ip, &new_dst_ip, &new_src_port, &new_dst_port);
const uint64_t new_hash = detail::hash_tuple(
    new_src_ip, new_dst_ip, new_src_port, new_dst_port);

for (std::size_t i = 0; i < n_entries_; ++i) {
    if (entries_[i].obj == static_cast<void*>(obj)) {
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "DpdkPoller::add: already registered"});
    }
    // 快速过滤：hash 不等直接跳；相等再做完整 4 字段比对（hash 冲突极少）
    if (entries_[i].conn_hash == new_hash &&
        entries_[i].src_ip   == new_src_ip &&
        entries_[i].dst_ip   == new_dst_ip &&
        entries_[i].src_port == new_src_port &&
        entries_[i].dst_port == new_dst_port) {
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "DpdkPoller::add: 4-tuple already registered"});
    }
}

// 下面保留：安装 entry（直接复用上面取好的 4-tuple + hash，
// 不再重复调用 tuple_fn 一次）
PollableEntry& entry = entries_[n_entries_];
entry.obj             = static_cast<void*>(obj);
entry.process_burst_fn = /* ... */;
entry.detach_fn        = /* ... */;
entry.tuple_fn         = /* ... */;
entry.src_ip    = new_src_ip;
entry.dst_ip    = new_dst_ip;
entry.src_port  = new_src_port;
entry.dst_port  = new_dst_port;
entry.conn_hash = new_hash;
++n_entries_;
obj->notify_attached_(this);
```

### `pick_src_port` 实现骨架

```cpp
template <class P>
std::expected<uint16_t, core::ErrorInfo>
DpdkPoller<P>::pick_src_port(uint32_t src_ip,
                              uint32_t dst_ip,
                              uint16_t dst_port,
                              uint16_t range_begin,
                              uint16_t range_end,
                              uint16_t preferred) const noexcept {
    auto* log = detail::poller_logger();

    // ── 参数校验 ─────────────────────────────────────────────────────────
    if (range_begin < 1024) {
        SPDLOG_LOGGER_WARN(log,
            "DpdkPoller::pick_src_port: range_begin={} below 1024",
            range_begin);
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "DpdkPoller::pick_src_port: range_begin must be >= 1024"});
    }
    if (range_begin > range_end) {
        SPDLOG_LOGGER_WARN(log,
            "DpdkPoller::pick_src_port: inverted range [{}, {}]",
            range_begin, range_end);
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "DpdkPoller::pick_src_port: range_begin > range_end"});
    }
    if (preferred != 0 &&
        (preferred < range_begin || preferred > range_end)) {
        SPDLOG_LOGGER_WARN(log,
            "DpdkPoller::pick_src_port: preferred={} outside [{}, {}]",
            preferred, range_begin, range_end);
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "DpdkPoller::pick_src_port: preferred out of range"});
    }

    // ── 局部谓词：4-tuple 是否与已注册 entry 冲突 ─────────────────────────
    auto is_in_use = [&](uint16_t candidate) noexcept -> bool {
        for (std::size_t i = 0; i < n_entries_; ++i) {
            const auto& e = entries_[i];
            if (e.src_ip   == src_ip   &&
                e.dst_ip   == dst_ip   &&
                e.dst_port == dst_port &&
                e.src_port == candidate) {
                return true;
            }
        }
        return false;
    };

    // ── 软偏好 fast-path ────────────────────────────────────────────────
    if (preferred != 0 && !is_in_use(preferred)) {
        SPDLOG_LOGGER_DEBUG(log,
            "DpdkPoller::pick_src_port: preferred={} accepted", preferred);
        return preferred;
    }

    // ── 随机起点 + 线性探测 ──────────────────────────────────────────────
    const uint32_t range = static_cast<uint32_t>(range_end - range_begin) + 1u;
    uint32_t seed = 0;
    if (::getrandom(&seed, sizeof(seed), 0) != sizeof(seed)) {
        // getrandom 在 Linux ≥ 3.17 从不失败 — 保底记 ERROR 再继续 with 0
        SPDLOG_LOGGER_ERROR(log,
            "DpdkPoller::pick_src_port: getrandom failed, falling back to 0");
        seed = 0;
    }
    const uint32_t start = seed % range;
    for (uint32_t i = 0; i < range; ++i) {
        const uint16_t candidate =
            static_cast<uint16_t>(range_begin + ((start + i) % range));
        if (!is_in_use(candidate)) {
            SPDLOG_LOGGER_DEBUG(log,
                "DpdkPoller::pick_src_port: selected port={} after {} probes",
                candidate, i + 1);
            return candidate;
        }
    }

    SPDLOG_LOGGER_WARN(log,
        "DpdkPoller::pick_src_port: no free port in [{}, {}] for "
        "src_ip=0x{:08x} dst_ip=0x{:08x} dst_port={}",
        range_begin, range_end, src_ip, dst_ip, dst_port);
    return std::unexpected(core::ErrorInfo{
        core::Error::OutOfMemory,
        "DpdkPoller::pick_src_port: no free port in range"});
}
```

**LOC 估算**（实现 + 注释）：≈ 75 行

### 数据流

```
 User                                    Poller
  │                                        │
  │  pick_src_port(src_ip, dst_ip, 443)    │
  ├───────────────────────────────────────▶│
  │                                        │ validate range
  │                                        │ [preferred fast-path?]
  │                                        │ getrandom → seed
  │                                        │ probe entries_ linearly
  │◀────────── uint16_t port ──────────────┤
  │                                        │
  │  cfg.legacy.tuple.src_port = port      │
  │  DpdkTcpStream::create(cfg)            │
  │   → TcpConfig::validate() PASSES       │
  │     (src_port now non-zero)            │
  │   → TCP 3-way / TLS / WS               │
  │  stream created                        │
  │                                        │
  │  poller->add(stream.get())             │
  ├───────────────────────────────────────▶│
  │                                        │ authoritative 4-tuple check
  │                                        │ (race race vs pick_src_port
  │                                        │  → possible InvalidConfig
  │                                        │   but single-thread use = 0%)
  │◀────────── OK or InvalidConfig ────────┤
  │                                        │
  │  [on failure: drop stream, retry       │
  │   pick_src_port, create again]         │
  │                                        │
  │  ~stream                               │
  │  │ auto-detach                         │
  │  │                                     │ entries_ shrinks
```

---

## 接口设计

### 公共 API

| API | 签名 | 行为 |
|-----|------|------|
| 既有 `DpdkPoller::add` | `add(P*) → expected<void, ErrorInfo>` | **扩展**：除现有 obj 重复检查外，新增 4-tuple 重复检查，detail 区分两种错误 |
| 新增 `DpdkPoller::pick_src_port` | 见上 | 建议性查询，不修改 Poller 状态，返回候选端口或错误 |
| `DpdkTcpStream::create` | 不变 | 保持单一签名；用户把 pick 结果写入 `cfg.legacy.tuple.src_port` |

### 错误体系

沿用 `core::Error` + `core::ErrorInfo`：

| 场景 | code | detail |
|------|------|--------|
| `add` obj 重复 | `Error::InvalidConfig` | `"DpdkPoller::add: already registered"`（既有，不变） |
| `add` 4-tuple 重复 | `Error::InvalidConfig` | `"DpdkPoller::add: 4-tuple already registered"`（**新增**） |
| `pick_src_port` range_begin < 1024 | `Error::InvalidConfig` | `"DpdkPoller::pick_src_port: range_begin must be >= 1024"` |
| `pick_src_port` range inverted | `Error::InvalidConfig` | `"DpdkPoller::pick_src_port: range_begin > range_end"` |
| `pick_src_port` preferred 越界 | `Error::InvalidConfig` | `"DpdkPoller::pick_src_port: preferred out of range"` |
| `pick_src_port` range 耗尽 | `Error::OutOfMemory` | `"DpdkPoller::pick_src_port: no free port in range"` |

---

## 编码规范

| 维度 | 规范 |
|------|------|
| 命名 | `pick_src_port` 是 Poller 成员方法，snake_case 与既有 `notify_attached_` / `process_burst_` 等友元 hook 一致（但因为是 public，不带尾下划线） |
| 错误处理 | `noexcept` + `std::expected<uint16_t, core::ErrorInfo>` |
| 日志 | 复用 `detail::poller_logger()`；DEBUG 记录成功路径（含 probe 次数），WARN 记录所有错误分支（含 range 上下文、src_ip/dst_ip/dst_port），ERROR 只留给 getrandom 失败这种理论上不该发生的事 |
| 注释 | `///` doxygen 块在 header 的方法声明处，说明使用模式、线程安全、错误条件；实现内只注关键段落 |
| 测试命名 | `TEST(DpdkPoller, <Scenario>)` 追加到现有 `test_dpdk_poller.cpp` 而不是新建文件 |

### CLAUDE.md 观测性对齐

- `pick_src_port` 和新扩展的 `add` 分支全部带 leveled logging
- WARN 分支包含 range、src_ip/dst_ip、n_entries_ 等可操作上下文（不是空洞的 "error occurred"）
- DEBUG 成功日志记录 probe 次数 —— 便于通过日志观察到分布是否接近 1（稀疏）还是接近 range（几乎满）

---

## 实施计划

> **Commit 策略**：每阶段独立 commit。每阶段完成并通过测试后才进入下一阶段，确保每个 commit 都是可回滚点。

### 阶段 1: `DpdkPoller::add()` 4-tuple 唯一性检查

- **交付物**：
  - `eph-net-dpdk/include/eph/net/dpdk/poller.hpp` 中 `add()` 方法的修改（将取 tuple 提前到扫描前，把 4-tuple 比对合并进同一次线性扫描）
  - `eph-net-dpdk/tests/test_dpdk_poller.cpp` 追加 test case：
    - `TEST(DpdkPoller, AddRejectsDuplicateFourTuple)` —— 两个不同对象，`tuple_for_poller_` 返回相同 4-tuple，第二次 `add` 返回 `InvalidConfig` with detail `"4-tuple already registered"`
    - `TEST(DpdkPoller, AddAcceptsDistinctFourTuplesOnSameDst)` —— 同 dst_ip/dst_port，不同 src_port，两次 add 都成功（sanity guard 防止新检查过度拒绝）
- **验收标准**：
  - `xmake run test_dpdk_poller` 全绿
  - 原有 test case 全部保持通过
  - asan 模式编译 + 运行无 warning
- **不变量**：
  - `DpdkPoller::add` 的公共签名不变
  - `DpdkPoller::remove` 路径不变
  - entries_ 存储布局不变
- **预估**：~25 LOC 生产代码 + ~60 LOC 测试
- **Commit 消息**：`fix(net-dpdk): reject duplicate 4-tuples in DpdkPoller::add`

### 阶段 2: `DpdkPoller::pick_src_port` 成员方法

- **交付物**：
  - `eph-net-dpdk/include/eph/net/dpdk/poller.hpp` 新增 `pick_src_port` 方法（见上文实现骨架）
  - 必要的 header include：`<sys/random.h>`（若未包含）
  - `eph-net-dpdk/tests/test_dpdk_poller.cpp` 追加 test case 列表（见下）
- **测试 case 列表**（追加到现有文件，全部必需）：

  | 测试名 | 场景 | 断言 |
  |--------|------|------|
  | `PickSrcPort_EmptyPollerReturnsInRange` | 空 Poller，`pick_src_port(...)` | 返回值 ∈ [32768, 60999] |
  | `PickSrcPort_PreferredAcceptedIfFree` | 空 Poller，preferred=40001 | 返回 40001 |
  | `PickSrcPort_PreferredOutOfRangeFails` | preferred=100 | InvalidConfig，detail 含 `"preferred out of range"` |
  | `PickSrcPort_InvertedRangeFails` | range_begin=50000, range_end=40000 | InvalidConfig，detail 含 `"range_begin > range_end"` |
  | `PickSrcPort_PrivilegedRangeFails` | range_begin=100 | InvalidConfig，detail 含 `"range_begin must be >= 1024"` |
  | `PickSrcPort_SkipsRegisteredFourTuple` | 注册 1 个 stream（src_port=40001 与目标 4-tuple 其他字段相同）→ pick 应跳过 40001 | 返回值 ≠ 40001，但仍 ∈ 范围 |
  | `PickSrcPort_PreferredTakenDowngradesToRandom` | 注册 stream 用 preferred=40001，再 pick(preferred=40001) | 返回值 ≠ 40001（软约束降级） |
  | `PickSrcPort_DifferentDstDoesNotConflict` | 注册 src_port=40001 dst_port=443；pick(..., dst_port=8080, preferred=40001) | 返回 40001（不同 4-tuple，不冲突） |
  | `PickSrcPort_ExhaustionReturnsOutOfMemory` | range={50000, 50002}（3 个端口），注册 3 个 stream 占满这 3 个端口，再 pick | OutOfMemory，detail 含 `"no free port in range"` |
  | `PickSrcPort_ConfirmedByAdd` | pick → 把返回 port 放进一个 fake stream 的 tuple_for_poller_ → `poller->add()` 成功 | 测试 pick + add 的端到端串行正确性 |
- **验收标准**：
  - `xmake run test_dpdk_poller` 全绿
  - asan 模式编译 + 运行无 warning（可选 tsan —— 因为 pick_src_port 明确非 MT-safe，tsan 不测并发）
  - 实现本体 LOC ≤ 80（含注释；软目标）
- **推荐 skill**：直接 Edit + Write
- **预估**：~75 LOC 实现 + ~160 LOC 测试
- **Commit 消息**：`feat(net-dpdk): add DpdkPoller::pick_src_port helper for client source port selection`

### 阶段 3: CHANGELOG + 回归

- **交付物**：
  - `eph-net-dpdk/CHANGELOG.md` 新增一行："`DpdkPoller::add` now rejects duplicate 4-tuples; new `DpdkPoller::pick_src_port` helper returns an unused ephemeral source port for TCP client connections."
  - 全量模块测试：`xmake build -g tests && xmake run -g tests`（确认无回归）
  - 现有 bench 目标的抽样构建（不运行）：`xmake build bench_fix_parse`（确认 header 变更没 broken 远端）
  - DPDK 路径构建：`xmake f --toolchain=gcc14 --runtimes=stdc++ && xmake build eph-net-dpdk`（确认 DPDK 编译 pipeline 不被打破）
- **验收标准**：
  - 全量测试绿
  - DPDK 构建成功
  - `grep -n "DpdkTcpStream::create" eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp` 只返回原来的单个签名（证明 stream 接口零改动）
  - `TcpConfig::validate()` 的 src_port != 0 约束字符串在 tcp.hpp 中不变（`grep "src_port must be explicit"`）
- **Commit 消息**：`docs(net-dpdk): changelog for pick_src_port and poller 4-tuple check`

---

## 关键决策记录

### D-1: 为什么不用独立 `SourcePortPool` 类

- **问题**：是否需要一个持有位图 + grace 队列的独立 pool 类？
- **选项**：
  - A. 独立 `SourcePortPool` / `SourcePortLease` 类型，RAII，2MSL grace
  - B. `DpdkPoller::pick_src_port` 成员方法，无状态
- **决策**：**B**
- **理由**：
  1. 阶段 1 的 `DpdkPoller::add` 4-tuple 检查已经是权威冲突判定器 —— 独立 pool 会成为该权威的冗余副本（single-source-of-truth 违反）
  2. 28k 端口范围下随机撞 TIME_WAIT 的概率是 0.0036%，不值得位图 + grace 的实现成本
  3. Poller 本身不是 MT-safe，pool 的 `std::mutex` 是在保护不存在的并发场景
  4. 用户已经得到了所有想要的行为：Tokio-style 便利（一行 `pick_src_port`）、flow-director 硬编码（老路径）、失败后重试（再次调 pick）
  5. 规模从 ~730 LOC 降到 ~220 LOC，R2 的复杂度预算被严格守住
- **验收标准**：plan 中不存在 `SourcePortPool` / `SourcePortLease` 任何字样（除本决策记录）；`DpdkTcpStream` 签名不变

### D-2: `pick_src_port` 为什么挂在 `DpdkPoller` 而不是自由函数

- **问题**：作为 Poller 成员方法还是命名空间自由函数？
- **选项**：
  - A. `DpdkPoller::pick_src_port()` 成员方法
  - B. 自由函数 `eph::net::dpdk::pick_src_port(const DpdkPoller&, ...)`
- **决策**：**A**
- **理由**：成员方法有直接访问 `entries_` 私有成员的便利；自由函数需要暴露 entries 的 iteration 接口（扩大公共 API 表面）。方法形态也符合 "对象知道自己状态" 的 OOP 直觉。
- **验收标准**：`DpdkPoller` 类定义中出现 `pick_src_port` 方法

### D-3: 随机起点 vs 顺序起点

- **问题**：探测从 `range_begin` 顺序开始，还是从随机点开始？
- **选项**：
  - A. 顺序起点（每次从 range_begin 开始扫描）
  - B. 随机起点 + 环形探测
- **决策**：**B**
- **理由**：顺序起点会让低端端口的重新分配概率显著高于高端，容易撞上刚释放的端口（对端 TIME_WAIT）。随机起点把被撞概率平均到整个 range，最大化 28k 范围的统计优势 —— 这正是我们"省掉 grace 窗口"的理由的技术前提。`getrandom(2)` 已经在项目其他地方被用（ISN 生成），无新依赖。
- **验收标准**：实现使用 `getrandom` + 模运算起点；测试 `PickSrcPort_EmptyPollerReturnsInRange` 反复调用 N 次，观察返回值应分布在整个 range（smoke check，不做严格分布测试）

### D-4: `preferred` 作为可选参数 vs 独立方法

- **问题**：软偏好应该作为 `pick_src_port` 的可选参数，还是拆成独立 `pick_src_port_preferred(p)` 方法？
- **选项**：
  - A. 可选参数 `preferred = 0`
  - B. 两个独立方法
- **决策**：**A**
- **理由**：R14 在讨论中反对布尔参数代码异味，但 `uint16_t preferred` 不是布尔 —— 它有自然的空值 (`0`，因为 `0` 在 range_begin=1024 之下本就非法)。一个方法少一份文档、少一份测试样板。R14 的论点针对的是 `bool strict = false` 的歧义，不适用这里。
- **验收标准**：单个 `pick_src_port` 方法，`preferred` 参数默认 `0`

### D-5: 不提供硬约束 `pick_src_port_exact`

- **问题**：是否需要一个 "只要 preferred，拿不到就失败" 的硬约束版本？
- **选项**：
  - A. 提供（参照讨论中 R3 要求的 `acquire_exact`）
  - B. 不提供
- **决策**：**B**
- **理由**：R3 的硬约束需求针对的是 flow-director preregister —— 该场景下用户本来就走老 `create(cfg)` + 硬编码 src_port 路径，压根不调 `pick_src_port`。`pick_src_port` 的定位是 "便利层"，便利层不需要提供硬约束形态。若用户确实需要"严格要 port p"，他们可以自己写：`if (port != preferred) return error` —— 一行代码。
- **验收标准**：plan 不包含 `pick_src_port_exact` 签名

### D-6: `pick_src_port` 声明为 `const` 成员

- **问题**：方法要不要 const-qualified？
- **选项**：
  - A. `const` —— 只读查询
  - B. 非 `const` —— 允许未来加内部状态
- **决策**：**A**
- **理由**：方法不修改 Poller 任何状态（entries_ 只读扫描），const 对用户是正确的契约承诺。未来如需增加内部状态应作为独立变更讨论。
- **验收标准**：header 中方法签名带 `const`

---

## 风险与缓解

| 风险 | 可能性 | 影响 | 缓解 |
|------|--------|------|------|
| pick → create → add 之间有并发 add 抢占同 4-tuple | 极低 | `add` 返回 InvalidConfig，用户需重试 | 文档明确 "advisory query"；单 driver 线程用法下该场景为 0；即使发生，失败路径已由 add 的检查兜住 |
| 随机起点让测试 `PickSrcPort_EmptyPollerReturnsInRange` 难以断言具体值 | 中 | 测试 flakiness | 测试只断言返回值 ∈ 范围，不断言具体值；重复采样验证分布是 smoke check 不是严格分布 |
| `getrandom(2)` 在非 Linux 失败 | 项目 Linux-only | N/A | CLAUDE.md 明确 Linux 平台；`getrandom` 返回值被检查，失败降级到 seed=0（日志 ERROR） |
| 4-tuple 扫描成本随 n_entries 线性增长 | 低 | `pick_src_port` 最坏 O(range × n_entries)；range=28k，n_entries ≤ kMaxConn | 冷路径，连接建立时；kMaxConn 为编译期小常数；与 `add` 本身的扫描成本同量级 |
| 用户误以为 `pick_src_port` 保证返回的端口之后一定能成功 `add` | 中 | 逻辑 bug | 文档明确 advisory + 给出完整 usage 示例，包含 add 的失败处理；测试 `PickSrcPort_ConfirmedByAdd` 演示正确用法 |

---

## 一致性检查

- [x] `TcpConfig::validate()` 保持 `src_port != 0` 约束 —— 用户写入 pick 返回值后再走 validate，自然通过
- [x] Kernel backend 零改动
- [x] Bench 客户端零改动
- [x] Header-only 约定 —— 修改全在 `.hpp`，无 `.cpp`
- [x] 错误类型统一 —— 全部 `core::ErrorInfo`
- [x] 观测性（CLAUDE.md）—— `pick_src_port` 所有分支有 leveled logging，WARN 带上下文
- [x] 测试命名（CLAUDE.md）—— 场景驱动
- [x] 与讨论收敛兼容：
  - R3 flow-director preregister：老路径零影响 ✓
  - R14 显式所有权：Poller 为唯一权威 ✓
  - R2 复杂度预算：~220 LOC vs 原 ~730 LOC ✓
  - R1 硬条件 (a)：阶段 1 落地 ✓
- [x] `DpdkTcpStream::create` 签名不变 —— 阶段 3 的 grep 验证
- [x] 公共 API 新增最小化：1 个成员方法，0 个新类型

---

## 后续（本 plan 外）

- 若 `DpdkUdpSocket` 未来也需要 client 场景的源端口选择：在 `DpdkPoller` 上扩展 `pick_src_port` 支持指定 L4 协议，或新增 `pick_src_port_udp`（语义完全相同，只是 4-tuple 语义细节不同）
- 若出现真实部署撞 TIME_WAIT 的观测证据：首先考虑扩大 range（默认已是 Linux 的 `ip_local_port_range` 28k 上限），其次再考虑在 Poller 内加 small LRU-style grace set（不是位图），粒度远小于原 SourcePortPool 方案
- 真实 NIC 下 `pick_src_port → create → add` 的端到端 E2E 测试，扩展 `tests/integration/test_dpdk_e2e.cpp`（本 plan 暂不要求）
