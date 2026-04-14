# Design Report: DPDK Source Port Selection Helper

## 概况

| 项 | 值 |
|----|-----|
| 时间 | 2026-04-14 02:50 – 03:20 |
| 耗时 | ~30 分 |
| 模式 | auto (blueprint-driven) |
| 需求 | 为 DPDK TcpStream client 提供合理端口选择能力，同时修复 Poller 的 4-tuple 唯一性空洞 |
| 讨论轮数 | 0（blueprint 已收敛，直接实施） |
| 参与角色 | — |
| 提交 | `3f5424c` + `1a9bb29` + `df9608c` |
| Blueprint | `.artifacts/plan-dpdk-source-port-pool-20260414-025600.md`（已确认 + 简化修订） |
| 先行讨论 | `.artifacts/discuss-dpdk-tcp-src-port-allocation-20260414-025000.md` |

## 需求边界

**In scope**：

- `DpdkPoller::add()` 4-tuple 唯一性检查（前置修复）
- `DpdkPoller::pick_src_port()` advisory 查询方法
- 单元测试：新增 12 个 case

**Out of scope**（严格遵守）：

- `SourcePortPool` / `SourcePortLease` 独立类型 —— 初版 plan 已被简化否决
- `DpdkTcpStream::create` 重载 —— 签名不变
- `TcpConfig::validate()` 约束放宽 —— `src_port != 0` 保留
- kernel backend 对称改造
- bench 客户端迁移

## 设计方案

**核心思想**：`DpdkPoller::entries_` 是已注册连接的**单一权威**。新增 `pick_src_port()` 作为 advisory 查询，扫描 entries_ 并返回一个不与任何已注册 4-tuple 冲突的端口；`add()` 是最终的权威检查（通过 stage 1 的 4-tuple 唯一性扩展）。不引入任何独立 bookkeeping 数据结构 —— 没有位图、没有 grace 队列、没有锁、没有新类型。

### 关键设计决策

| 决策 | 选择 | 否决项 | 理由 |
|------|------|--------|------|
| 状态归属 | Poller entries_ 作为唯一权威 | 独立 SourcePortPool 位图 | 避免两个数据源同步问题；single source of truth |
| 2MSL grace | 不做 | 位图 + FIFO grace 队列 | 28k range 下随机撞车 0.0036%，不值得 |
| 探测起点 | `getrandom(2)` 随机 | 顺序扫描 | 随机分布被撞概率（免 grace 的统计学前提） |
| `preferred` 形态 | 可选参数 `= 0` | 独立 `pick_src_port_preferred` 方法 | `0` 是天然哨兵（1024 以下非法）；单方法减半文档/测试成本 |
| 硬约束版本 | 不提供 | `pick_src_port_exact` | 硬约束用户走老 `create(cfg)` 硬编码路径，不碰 helper |
| 挂载点 | `DpdkPoller` 成员方法 | 自由函数 `pick_src_port(poller, ...)` | 成员方法直接访问 entries_，不扩大 public API 表面 |
| const 修饰 | `const` 成员 | 非 const | 纯查询，不修改状态；契约更清晰 |
| 线程安全 | 不加锁 | `std::mutex` 保护 | `DpdkPoller` 本身非 MT-safe，加锁是为不存在的场景做保护 |

### 最终接口

```cpp
// 前置修复：add() 现在也检查 4-tuple 冲突
template <DpdkPollable P>
[[nodiscard]] std::expected<void, core::ErrorInfo> add(P* obj) noexcept;

// 新增 helper
[[nodiscard]] std::expected<uint16_t, core::ErrorInfo>
pick_src_port(uint32_t src_ip,
              uint32_t dst_ip,
              uint16_t dst_port,
              uint16_t range_begin = 32768,
              uint16_t range_end   = 60999,
              uint16_t preferred   = 0) const noexcept;
```

**典型用法**：

```cpp
auto port = poller->pick_src_port(src_ip, dst_ip, 443).value();
cfg.legacy.tuple.src_port = port;
auto stream = DpdkTcpStream::create(std::move(cfg)).value();
auto add_r  = poller->add(stream.get());  // authoritative check
```

## 实现概况

### 修改文件

| 文件 | 动作 | 增量 |
|------|------|------|
| `eph-net-dpdk/include/eph/net/dpdk/poller.hpp` | 修改 | +164 行（add 4-tuple 检查 + pick_src_port 方法 + 主模板 forward） |
| `eph-net-dpdk/tests/test_dpdk_poller.cpp` | 修改 | +242 行（12 个新 case） |
| `eph-net-dpdk/CHANGELOG.md` | 修改 | +36 行 |

### 测试覆盖

| 测试 | 断言 |
|------|------|
| `AddRejectsDuplicateFourTuple` | 两个 obj 同 4-tuple → 第二次 add 返回 `InvalidConfig` with detail 含 `"4-tuple"` |
| `AddAcceptsDistinctFourTuplesOnSameDst` | 同 dst_ip/dst_port 不同 src_port → 两次 add 都成功（不过度拒绝） |
| `PickSrcPort_EmptyPollerReturnsInRange` | 空 Poller，返回值 ∈ [32768, 60999] |
| `PickSrcPort_PreferredAcceptedIfFree` | preferred=40001，返回 40001 |
| `PickSrcPort_PreferredOutOfRangeFails` | preferred=100 → `InvalidConfig` |
| `PickSrcPort_InvertedRangeFails` | range=[50000, 40000] → `InvalidConfig` |
| `PickSrcPort_PrivilegedRangeFails` | range_begin=100 → `InvalidConfig` |
| `PickSrcPort_SkipsRegisteredFourTuple` | 注册 src_port=40001，preferred=40001 → 返回 ≠ 40001 但 ∈ 范围 |
| `PickSrcPort_DifferentDstDoesNotConflict` | 注册 dst_port=443，查 dst_port=8080 preferred=40001 → 返回 40001 |
| `PickSrcPort_ExhaustionReturnsOutOfMemory` | range=[50000,50002] 全满 → `OutOfMemory` |
| `PickSrcPort_ConfirmedByAdd` | pick → add 端到端串行正确 |
| `PickSrcPort_RandomStartSpreadsPicks` | 32 次采样 seen.size() > 1（smoke check 分布） |

全部 20 个 `DpdkPoller` 测试通过（原有 8 + 新增 12）。

### 回归结果

| 测试目标 | 结果 |
|---------|------|
| `test_dpdk_poller` | ✅ 20 passed |
| `test_dpdk_tcp_stream` | ✅ 8 passed |
| `test_dpdk_udp_socket` | ✅ 3 passed |
| `test_dpdk_udp_multicast` | ✅ 3 passed |
| `test_dpdk_reasm_overflow` | ✅ 5 passed |
| `test_dpdk_fault_tolerance` | ✅ 10 passed |
| `test_dpdk_tls_state` | ✅ 2 passed |
| `test_dpdk_tls_handshake` | ⚠️ 预先存在的 aws-lc linker 错误（需 gcc14-wrap，env 问题，baseline 也失败） |
| `test_tls_record` (eph-net) | ⚠️ 预先存在的 test code 错误（使用 `.empty()`/`.find()` on std::expected，与本 plan 无关） |

本 plan 涉及文件的所有可运行测试 **51 全绿，0 回归**。

## 不变量校验

- ✅ `grep -c "create(StreamConfig" eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp` == 1（DpdkTcpStream 签名不变）
- ✅ `grep "src_port must be explicit" eph-net-dpdk/include/eph/dpdk/tcp.hpp` 命中（validate 约束保留）
- ✅ `grep -r "SourcePortPool" eph-net-dpdk/` 无命中（初版设计被完全弃用）
- ✅ kernel backend 零改动（`eph-net-kernel/` 无修改）
- ✅ bench 客户端零改动（`benchmarks/latency/` 无修改）
- ✅ legacy 测试不引用 DpdkPoller（范围隔离正确）

## 对比初版 plan

| 维度 | 初版（SourcePortPool） | 实施版（pick_src_port） |
|------|----------------------|----------------------|
| 新增类型 | 3（Pool/Lease/PoolConfig） | **0** |
| 新增文件 | 4 | **0** |
| 修改文件 | 3 | 3 |
| 实现 LOC | ~230 | ~90（方法本体） |
| 测试 LOC | ~500 | ~242 |
| Commit 数 | 6 | **3** |
| 新类型心智负担 | lease 生命周期、pool 所有权、grace 窗口 | 零 |

## 后续建议

**可选后续工作**（本 plan 外，可 `/improve` 或独立 `/design`）：

- 真实 NIC 下的 `pick_src_port → create → add` 端到端测试，加入 `tests/integration/test_dpdk_e2e.cpp`
- `DpdkUdpSocket` 的对称 client 场景：`pick_src_port` 的语义对 TCP/UDP 完全一致，可直接复用，无需新 API
- 若出现真实部署撞 TIME_WAIT 的观测证据：首选扩大 range（默认已是 Linux 上限 28k），次选在 Poller 内加 small LRU-style grace set（粒度远小于原 SourcePortPool 方案）

**环境层面 TODO**（与本 plan 无关但已发现）：

- `test_tls_record.cpp` 1242 行前后的 `std::expected` 误用是编译 broken，需单独修（`/fix`）
- `test_dpdk_tls_handshake` 的 aws-lc 链接顺序问题需要配置 `xmake f --toolchain=gcc14-wrap` 或修 xmake.lua 的 link order

## 核心判断回顾

用户在 plan 阶段提出的质疑 ——"只提供 helper 函数，让用户拿到可用端口即可，因为已经有唯一性检查了，不是吗？"—— 是本次设计最关键的一次方向修正。这个质疑击中了初版 SourcePortPool 方案的根本缺陷：**当 Poller::add 被升级为权威的 4-tuple 检查器后，任何独立的端口 bookkeeping 数据结构都是冗余的**。从 ~730 LOC 降到 ~330 LOC，从 3 个新类型降到 0 个新类型，功能完全等价。

Single source of truth 是本次最核心的设计收获。
